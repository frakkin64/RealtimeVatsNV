#pragma once

namespace Plugin::RealtimeVats
{
	// installs the key, per-frame and projectile aim hooks
	void Install();

	// NVSE messages: frees the game's own bindings of the plugin keys while locked and restores them afterwards
	void OnMessage(NVSE::MessagingInterface::Message* a_message);
}
