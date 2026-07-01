/*************************************************************************
*    UrBackup - Client/Server backup system
*    NeoBackup Ransom Defender - Physical Confirmation Gate
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
**************************************************************************/
#pragma once

#include <string>
#include <vector>
#include "../Interface/Thread.h"
#include "../Interface/Mutex.h"
#include "../Interface/Condition.h"

// Physical Confirmation Gate: destructive operations (e.g. manual snapshot
// deletion) are queued as PENDING and only committed when the physical button
// (GPIO 26) is pressed within a timeout window. Otherwise the request is
// discarded automatically.
class PhysicalGate
{
public:
	enum EGateResult
	{
		EGateResult_Approved,	// physical button pressed within timeout
		EGateResult_Timeout,	// no physical confirmation in time
		EGateResult_Disabled	// gate disabled -> caller should proceed (no gating)
	};

	// Initialize the gate singleton and start the GPIO watcher thread.
	// Reads configuration from server parameters / environment.
	static void init();

	// Tear down the gate (stop watcher thread). Safe to call on shutdown.
	static void destroy();

	// Returns true if the gate is enabled (i.e. destructive ops are gated).
	static bool isEnabled();

	// Request approval for a destructive operation. Blocks until the physical
	// button is pressed (EGateResult_Approved) or the timeout elapses
	// (EGateResult_Timeout). If the gate is disabled, returns
	// EGateResult_Disabled immediately so the caller proceeds without gating.
	//
	// description: human-readable summary for the audit log, e.g.
	//   "delete file backup id=42 client='exjang'".
	static EGateResult requestApproval(const std::string& description);

private:
	PhysicalGate();
	~PhysicalGate();

	static PhysicalGate* instance;

	EGateResult doRequestApproval(const std::string& description);

	// Called by the GPIO watcher when a physical button press is observed.
	void onButtonPressed();
	friend class GpioWatcherThread;

	void audit(const std::string& event, const std::string& description);

	IMutex* mutex;
	ICondition* cond;

	bool enabled;
	int timeout_ms;

	// Monotonic counter of observed button presses. A pending request records
	// the count at the time it started waiting; any increment satisfies it.
	unsigned int press_count;

	// Number of destructive requests currently waiting for confirmation.
	unsigned int pending_count;

	std::string audit_log_path;
	bool audit_warned;
	IThread* watcher_thread;
	volatile bool watcher_stop;
};
