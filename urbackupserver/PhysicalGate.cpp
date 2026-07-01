/*************************************************************************
*    UrBackup - Client/Server backup system
*    NeoBackup Ransom Defender - Physical Confirmation Gate
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
**************************************************************************/
#include "PhysicalGate.h"
#include "../Interface/Server.h"
#include "../stringtools.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifdef WITH_LIBGPIOD
#include <gpiod.h>
#endif

PhysicalGate* PhysicalGate::instance = NULL;

namespace
{
	// Defaults (overridable via server parameters / environment).
	const int default_timeout_ms = 90 * 1000; // 90s, per NeoBackup spec
	const int min_timeout_ms = 30 * 1000;
	const int max_timeout_ms = 300 * 1000;
	const unsigned int default_gpio_line = 26;	  // GPIO 26 (BCM), 40-pin header
	// Raspberry Pi 5: the 40-pin header GPIOs live on the RP1 controller, which
	// enumerates as gpiochip4 (pinctrl-rp1). gpiochip0 is the SoC-internal
	// controller on Pi 5. On Pi 4 and earlier the header is gpiochip0.
	const std::string default_gpio_chip = "gpiochip4";
	const std::string default_audit_log = "/var/log/urbackup_physical_gate.log";

	// ANSI color codes (only emitted when stderr is a TTY, e.g. the Pi console).
	bool stderr_is_tty()
	{
		return isatty(fileno(stderr)) != 0;
	}

	// Truncate/pad a line to the banner width so the box borders stay aligned.
	std::string banner_line(const std::string& s)
	{
		const size_t w = 52;
		std::string t = s;
		if (t.size() > w)
			t = t.substr(0, w - 1) + "~";
		return t;
	}

	// Big, eye-catching banner printed directly to the console (stderr), so a
	// terminal logged into the Pi shows physical events loudly. A timestamp line
	// is always appended automatically.
	// color: ANSI SGR sequence (e.g. "1;33"), ignored when not a TTY.
	// l4 may be empty.
	void console_banner(const std::string& color, const std::string& l1,
		const std::string& l2, const std::string& l3, const std::string& l4)
	{
		bool tty = stderr_is_tty();
		const std::string on = tty ? ("\033[" + color + "m") : "";
		const std::string off = tty ? "\033[0m" : "";

		time_t now = time(NULL);
		struct tm tmv;
		localtime_r(&now, &tmv);
		char tbuf[32];
		strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);

		std::string ts = std::string("[") + tbuf + "]";
		std::string tail = l4.empty() ? ts : (banner_line(l4) + "  " + ts);

		fprintf(stderr,
			"\n%s"
			"+========================================================+\n"
			"|  %-52s|\n"
			"|  %-52s|\n"
			"|  %-52s|\n"
			"|  %-52s|\n"
			"+========================================================+%s\n\n",
			on.c_str(),
			banner_line(l1).c_str(),
			banner_line(l2).c_str(),
			banner_line(l3).c_str(),
			tail.c_str(),
			off.c_str());
		fflush(stderr);
	}

	std::string env_key(const std::string& key)
	{
		std::string k = "NEOBACKUP_" + key;
		strupper(&k);
		return k;
	}

	// Read an int "server parameter" with env fallback and clamping.
	int read_int_param(const std::string& key, int def)
	{
		std::string v = Server->getServerParameter(key);
		if (v.empty())
		{
			const char* e = getenv(env_key(key).c_str());
			if (e != NULL)
				v = e;
		}
		if (v.empty())
			return def;
		return watoi(v);
	}

	std::string read_str_param(const std::string& key, const std::string& def)
	{
		std::string v = Server->getServerParameter(key);
		if (v.empty())
		{
			const char* e = getenv(env_key(key).c_str());
			if (e != NULL)
				v = e;
		}
		if (v.empty())
			return def;
		return v;
	}
}

#ifdef WITH_LIBGPIOD
// Watches GPIO 26 for falling edges (active-low button with pull-up) and
// notifies the gate on each press.
class GpioWatcherThread : public IThread
{
public:
	GpioWatcherThread(PhysicalGate* gate, std::string chip, unsigned int line)
		: gate(gate), chip_name(chip), line_offset(line)
	{
	}

	void operator()()
	{
		struct gpiod_chip* chip = gpiod_chip_open_by_name(chip_name.c_str());
		if (chip == NULL)
		{
			Server->Log("PhysicalGate: cannot open gpio chip '" + chip_name +
				"'. Physical button DISABLED; destructive ops will time out.", LL_ERROR);
			return;
		}

		struct gpiod_line* line = gpiod_chip_get_line(chip, line_offset);
		if (line == NULL)
		{
			Server->Log("PhysicalGate: cannot get gpio line " + convert((int)line_offset), LL_ERROR);
			gpiod_chip_close(chip);
			return;
		}

		// Button to GND with internal pull-up -> falling edge on press.
		if (gpiod_line_request_falling_edge_events_flags(line, "urbackup-physical-gate",
				GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0)
		{
			Server->Log("PhysicalGate: cannot request edge events on gpio line " +
				convert((int)line_offset) + " (errno=" + convert((int)errno) + ")", LL_ERROR);
			gpiod_chip_close(chip);
			return;
		}

		Server->Log("PhysicalGate: watching GPIO " + convert((int)line_offset) +
			" on " + chip_name + " for physical confirmations.", LL_WARNING);

		int64 last_press_ms = 0;
		const int64 debounce_ms = 250;

		while (!gate_stop())
		{
			struct timespec ts;
			ts.tv_sec = 1;
			ts.tv_nsec = 0;
			int r = gpiod_line_event_wait(line, &ts);
			if (r < 0)
			{
				Server->Log("PhysicalGate: gpio event wait error.", LL_WARNING);
				continue;
			}
			if (r == 0)
				continue; // timeout, re-check stop flag

			struct gpiod_line_event ev;
			if (gpiod_line_event_read(line, &ev) < 0)
				continue;

			int64 now = Server->getTimeMS();
			if (now - last_press_ms < debounce_ms)
				continue; // debounce
			last_press_ms = now;

			gate->onButtonPressed();
		}

		gpiod_line_release(line);
		gpiod_chip_close(chip);
	}

private:
	bool gate_stop();

	PhysicalGate* gate;
	std::string chip_name;
	unsigned int line_offset;
};
#endif // WITH_LIBGPIOD

PhysicalGate::PhysicalGate()
	: mutex(NULL), cond(NULL), enabled(false), timeout_ms(default_timeout_ms),
	  press_count(0), pending_count(0), audit_warned(false),
	  watcher_thread(NULL), watcher_stop(false)
{
}

PhysicalGate::~PhysicalGate()
{
	if (cond != NULL)
		Server->destroy(cond);
	if (mutex != NULL)
		Server->destroy(mutex);
}

void PhysicalGate::init()
{
	if (instance != NULL)
		return;

	instance = new PhysicalGate();
	instance->mutex = Server->createMutex();
	instance->cond = Server->createCondition();

	instance->timeout_ms = read_int_param("physical_gate_timeout_ms", default_timeout_ms);
	if (instance->timeout_ms < min_timeout_ms)
		instance->timeout_ms = min_timeout_ms;
	if (instance->timeout_ms > max_timeout_ms)
		instance->timeout_ms = max_timeout_ms;

	instance->audit_log_path = read_str_param("physical_gate_audit_log", default_audit_log);

	// Gate is enabled unless explicitly disabled. This is the safe default for
	// a Ransom Defender appliance: destructive ops require physical approval.
	std::string disabled = read_str_param("physical_gate_disabled", "false");
	bool want_enabled = !(disabled == "true" || disabled == "1");

#ifdef WITH_LIBGPIOD
	if (want_enabled)
	{
		std::string chip = read_str_param("physical_gate_gpio_chip", default_gpio_chip);
		int line = read_int_param("physical_gate_gpio_line", (int)default_gpio_line);
		instance->enabled = true;
		instance->watcher_thread = new GpioWatcherThread(instance, chip, (unsigned int)line);
		Server->createThread(instance->watcher_thread, "physical gate");
		Server->Log("PhysicalGate: ENABLED. Destructive operations require physical "
			"button confirmation (timeout " + convert(instance->timeout_ms / 1000) + "s).", LL_WARNING);
	}
	else
	{
		instance->enabled = false;
		Server->Log("PhysicalGate: explicitly DISABLED via configuration.", LL_WARNING);
	}
#else
	(void)want_enabled;
	instance->enabled = false;
	Server->Log("PhysicalGate: built WITHOUT libgpiod support; gate DISABLED. "
		"Destructive operations are NOT physically gated.", LL_WARNING);
#endif

	instance->audit("init", "physical gate initialized, enabled=" +
		std::string(instance->enabled ? "true" : "false"));
}

void PhysicalGate::destroy()
{
	if (instance == NULL)
		return;
	instance->watcher_stop = true;
	// Watcher thread polls the stop flag with a 1s timeout and exits on its own;
	// it deletes itself via the thread pool semantics used by createThread.
	instance = NULL;
}

bool PhysicalGate::isEnabled()
{
	return instance != NULL && instance->enabled;
}

void PhysicalGate::onButtonPressed()
{
	IScopedLock lock(mutex);
	++press_count;
	bool had_pending = pending_count > 0;
	cond->notify_all();
	Server->Log("PhysicalGate: physical button pressed (press #" +
		convert((int)press_count) + ").", LL_WARNING);

	// Bright green when a request is waiting (it will now commit), cyan otherwise.
	if (had_pending)
	{
		console_banner("1;32",
			">>> PHYSICAL BUTTON PRESSED <<<",
			"Pending destructive operation APPROVED.",
			"Committing now...",
			"");
	}
	else
	{
		console_banner("1;36",
			">>> PHYSICAL BUTTON PRESSED <<<",
			"No operation is pending right now.",
			"(press registered)",
			"");
	}
}

PhysicalGate::EGateResult PhysicalGate::requestApproval(const std::string& description)
{
	if (!isEnabled())
		return EGateResult_Disabled;

	return instance->doRequestApproval(description);
}

PhysicalGate::EGateResult PhysicalGate::doRequestApproval(const std::string& description)
{
	IScopedLock lock(mutex);

	unsigned int start_count = press_count;
	int64 deadline = Server->getTimeMS() + timeout_ms;

	audit("pending", description);
	Server->Log("PhysicalGate: PENDING destructive request awaiting physical "
		"confirmation: " + description, LL_WARNING);

	// Bright yellow: a destructive op is now blocked, press the button to allow it.
	console_banner("1;33",
		"*** PHYSICAL CONFIRMATION REQUIRED ***",
		description,
		"PRESS THE BUTTON to approve, or wait to cancel.",
		"timeout: " + convert(timeout_ms / 1000) + "s");

	++pending_count;

	while (press_count == start_count)
	{
		int64 remaining = deadline - Server->getTimeMS();
		if (remaining <= 0)
		{
			--pending_count;
			audit("timeout", description);
			Server->Log("PhysicalGate: TIMEOUT, discarding destructive request: " +
				description, LL_WARNING);
			console_banner("1;31",
				"### TIMEOUT - REQUEST DISCARDED ###",
				description,
				"No button press in time. Nothing was deleted.",
				"");
			return EGateResult_Timeout;
		}
		cond->wait(&lock, (int)remaining);
	}

	--pending_count;
	audit("approved", description);
	Server->Log("PhysicalGate: APPROVED by physical button: " + description, LL_WARNING);
	console_banner("1;32",
		"=== APPROVED - COMMITTING ===",
		description,
		"Physical confirmation accepted.",
		"");
	return EGateResult_Approved;
}

void PhysicalGate::audit(const std::string& event, const std::string& description)
{
	// Append-only audit log with fsync, per spec (§6 Audit log).
	FILE* f = fopen(audit_log_path.c_str(), "a");
	if (f == NULL)
	{
		// Warn once so a missing/unwritable audit log is visible in the main log.
		if (!audit_warned)
		{
			audit_warned = true;
			Server->Log("PhysicalGate: cannot write audit log '" + audit_log_path +
				"' (errno=" + convert((int)errno) + "). Ensure it exists and is "
				"owned by the server user.", LL_WARNING);
		}
		return;
	}

	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	char tbuf[32];
	strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv);

	fprintf(f, "%s\t%s\t%s\n", tbuf, event.c_str(), description.c_str());
	fflush(f);
	fsync(fileno(f));
	fclose(f);
}

#ifdef WITH_LIBGPIOD
bool GpioWatcherThread::gate_stop()
{
	return PhysicalGate::instance == NULL || PhysicalGate::instance->watcher_stop;
}
#endif
