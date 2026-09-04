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

// Physical Confirmation Gate: destructive operations (e.g. manual backup
// deletion) are only committed when a physical button is pressed within a
// timeout window. Otherwise the request is discarded.
//
// The button and the security chip that authenticates it live behind the
// neobackup-gated helper daemon, reached over a unix socket. The daemon owns
// the serial device and the vendor crypto library; this class only asks
// "may I proceed?" and never learns the chip protocol.
// See PROTOCOL.md in the neobackup-gated repository.
class PhysicalGate
{
public:
	enum EGateResult
	{
		EGateResult_Approved,	// button pressed and chip authenticated
		EGateResult_Denied,		// chip authentication failed, or gate unreachable
		EGateResult_Timeout,	// no physical confirmation in time
		EGateResult_Disabled	// gate disabled -> caller should proceed
	};

	// Read configuration from server parameters / environment.
	static void init();

	static void destroy();

	static bool isEnabled();

	// Blocks until the helper daemon reports the outcome. If the gate is
	// disabled, returns EGateResult_Disabled immediately.
	//
	// Any failure to reach the daemon is reported as EGateResult_Denied:
	// without a working gate we must not delete anything.
	//
	// description: human-readable summary for the audit log, e.g.
	//   "delete file backup id=42 client='exjang'".
	static EGateResult requestApproval(const std::string& description);

	// Short reason for the last denial, suitable for the web API
	// (e.g. "chip_auth_failed"). Empty when the last result was not a denial.
	static std::string lastDenyReason();

private:
	PhysicalGate();

	static PhysicalGate* instance;

	EGateResult doRequestApproval(const std::string& description);

	// Sends one request line and reads one response line. False when the
	// daemon could not be reached.
	bool transact(const std::string& request, std::string& response, int timeout_ms);

	bool enabled;
	int timeout_ms;
	std::string socket_path;
	std::string deny_reason;
};
