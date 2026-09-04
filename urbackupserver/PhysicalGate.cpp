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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

PhysicalGate* PhysicalGate::instance = NULL;

namespace
{
	const int default_timeout_ms = 90 * 1000; // 90s, per NeoBackup spec
	const int min_timeout_ms = 30 * 1000;
	const int max_timeout_ms = 300 * 1000;
	const std::string default_socket = "/run/neobackup-gate.sock";

	// Allow the daemon a little longer than the button window before we give up.
	const int socket_grace_ms = 15 * 1000;

	bool stderr_is_tty()
	{
		return isatty(fileno(stderr)) != 0;
	}

	std::string banner_line(const std::string& s)
	{
		const size_t w = 52;
		std::string t = s;
		if (t.size() > w)
			t = t.substr(0, w - 1) + "~";
		return t;
	}

	// Big, eye-catching banner on stderr so a console logged into the appliance
	// shows physical events loudly. A timestamp is appended automatically.
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

	int read_int_param(const std::string& key, int def)
	{
		std::string v = read_str_param(key, std::string());
		if (v.empty())
			return def;
		return watoi(v);
	}

	// Extracts a "key":"value" string from a flat JSON object. The daemon's
	// replies are small and fixed-shape, so a full parser would be overkill.
	std::string json_field(const std::string& src, const std::string& key)
	{
		std::string pat = "\"" + key + "\"";
		size_t p = src.find(pat);
		if (p == std::string::npos)
			return std::string();

		p = src.find(':', p + pat.size());
		if (p == std::string::npos)
			return std::string();

		p = src.find('"', p);
		if (p == std::string::npos)
			return std::string();

		size_t end = src.find('"', p + 1);
		if (end == std::string::npos)
			return std::string();

		return src.substr(p + 1, end - p - 1);
	}

	// The description ends up inside a JSON string literal.
	std::string json_escape(const std::string& s)
	{
		std::string out;
		out.reserve(s.size());
		for (size_t i = 0; i < s.size(); ++i)
		{
			unsigned char c = (unsigned char)s[i];
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += (char)c;
			}
			else if (c >= 0x20)
			{
				out += (char)c;
			}
		}
		if (out.size() > 512)
			out.resize(512);
		return out;
	}
}

PhysicalGate::PhysicalGate()
	: enabled(false), timeout_ms(default_timeout_ms), socket_path(default_socket)
{
}

void PhysicalGate::init()
{
	if (instance != NULL)
		return;

	instance = new PhysicalGate();

	instance->timeout_ms = read_int_param("physical_gate_timeout_ms", default_timeout_ms);
	if (instance->timeout_ms < min_timeout_ms)
		instance->timeout_ms = min_timeout_ms;
	if (instance->timeout_ms > max_timeout_ms)
		instance->timeout_ms = max_timeout_ms;

	instance->socket_path = read_str_param("physical_gate_socket", default_socket);

	// Enabled unless explicitly disabled: the safe default for a Ransom
	// Defender appliance is that destructive ops need physical approval.
	std::string disabled = read_str_param("physical_gate_disabled", "false");
	instance->enabled = !(disabled == "true" || disabled == "1");

	if (!instance->enabled)
	{
		Server->Log("PhysicalGate: explicitly DISABLED via configuration. "
			"Destructive operations are NOT physically gated.", LL_WARNING);
		return;
	}

	Server->Log("PhysicalGate: ENABLED via " + instance->socket_path +
		" (timeout " + convert(instance->timeout_ms / 1000) + "s).", LL_WARNING);

	// Probe once so a misconfigured or stopped daemon is visible at startup
	// rather than at the first deletion attempt.
	std::string response;
	if (!instance->transact("{\"cmd\":\"status\"}", response, 5000))
	{
		Server->Log("PhysicalGate: helper daemon not reachable at " +
			instance->socket_path + ". Destructive operations will be DENIED "
			"until neobackup-gated is running.", LL_ERROR);
		return;
	}

	std::string version = json_field(response, "version");
	if (version != "1")
	{
		Server->Log("PhysicalGate: helper daemon speaks protocol version '" +
			version + "', expected '1'. Destructive operations will be DENIED.",
			LL_ERROR);
		return;
	}

	Server->Log("PhysicalGate: helper daemon ready (device=" +
		json_field(response, "device") + ", chip=" + json_field(response, "chip") + ").",
		LL_WARNING);
}

void PhysicalGate::destroy()
{
	delete instance;
	instance = NULL;
}

bool PhysicalGate::isEnabled()
{
	return instance != NULL && instance->enabled;
}

std::string PhysicalGate::lastDenyReason()
{
	if (instance == NULL)
		return std::string();
	return instance->deny_reason;
}

bool PhysicalGate::transact(const std::string& request, std::string& response,
	int timeout_ms_)
{
	struct sockaddr_un addr;
	if (socket_path.size() + 1 > sizeof(addr.sun_path))
	{
		Server->Log("PhysicalGate: socket path too long: " + socket_path, LL_ERROR);
		return false;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	{
		Server->Log("PhysicalGate: socket() failed (errno=" +
			convert((int)errno) + ").", LL_ERROR);
		return false;
	}

	// The daemon blocks for the whole button window, so the read timeout must
	// cover it. Connect stays short: the socket is local and either there or not.
	struct timeval tv;
	tv.tv_sec = timeout_ms_ / 1000;
	tv.tv_usec = (timeout_ms_ % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
	{
		Server->Log("PhysicalGate: cannot connect to " + socket_path +
			" (errno=" + convert((int)errno) + ").", LL_ERROR);
		close(fd);
		return false;
	}

	std::string line = request + "\n";
	size_t sent = 0;
	while (sent < line.size())
	{
		ssize_t n = write(fd, line.data() + sent, line.size() - sent);
		if (n <= 0)
		{
			Server->Log("PhysicalGate: write to helper daemon failed.", LL_ERROR);
			close(fd);
			return false;
		}
		sent += (size_t)n;
	}

	response.clear();
	char buf[512];
	while (response.find('\n') == std::string::npos)
	{
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		response.append(buf, (size_t)n);
		if (response.size() > 8192)
			break;
	}

	close(fd);

	if (response.empty())
	{
		Server->Log("PhysicalGate: no response from helper daemon.", LL_ERROR);
		return false;
	}

	return true;
}

PhysicalGate::EGateResult PhysicalGate::requestApproval(const std::string& description)
{
	if (!isEnabled())
		return EGateResult_Disabled;

	return instance->doRequestApproval(description);
}

PhysicalGate::EGateResult PhysicalGate::doRequestApproval(const std::string& description)
{
	deny_reason.clear();

	Server->Log("PhysicalGate: PENDING destructive request awaiting physical "
		"confirmation: " + description, LL_WARNING);

	console_banner("1;33",
		"*** PHYSICAL CONFIRMATION REQUIRED ***",
		description,
		"PRESS THE BUTTON to approve, or wait to cancel.",
		"timeout: " + convert(timeout_ms / 1000) + "s");

	std::string request = "{\"cmd\":\"await_approval\",\"timeout_ms\":" +
		convert(timeout_ms) + ",\"desc\":\"" + json_escape(description) + "\"}";

	std::string response;
	if (!transact(request, response, timeout_ms + socket_grace_ms))
	{
		deny_reason = "gate_unreachable";
		Server->Log("PhysicalGate: DENIED (helper daemon unreachable): " +
			description, LL_ERROR);
		console_banner("1;31",
			"### GATE UNAVAILABLE - DENIED ###",
			description,
			"neobackup-gated is not responding. Nothing deleted.",
			"");
		return EGateResult_Denied;
	}

	std::string result = json_field(response, "result");

	if (result == "approved")
	{
		Server->Log("PhysicalGate: APPROVED by physical button: " + description,
			LL_WARNING);
		console_banner("1;32",
			"=== APPROVED - COMMITTING ===",
			description,
			"Button pressed and chip authenticated.",
			"");
		return EGateResult_Approved;
	}

	if (result == "timeout")
	{
		Server->Log("PhysicalGate: TIMEOUT, discarding destructive request: " +
			description, LL_WARNING);
		console_banner("1;31",
			"### TIMEOUT - REQUEST DISCARDED ###",
			description,
			"No button press in time. Nothing was deleted.",
			"");
		return EGateResult_Timeout;
	}

	deny_reason = json_field(response, "reason");
	if (deny_reason.empty())
		deny_reason = "gate_denied";

	Server->Log("PhysicalGate: DENIED (" + deny_reason + "): " + description, LL_ERROR);
	console_banner("1;31",
		"### DENIED - " + deny_reason + " ###",
		description,
		"Physical confirmation could not be established.",
		"");
	return EGateResult_Denied;
}
