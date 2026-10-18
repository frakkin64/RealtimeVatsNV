#include "Commands.h"
#include "Settings.h"

namespace
{
	void OnMessage(NVSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case NVSE::MessagingInterface::kPostLoad:
			REX::INFO("NVSE finished loading plugins");
			break;
		case NVSE::MessagingInterface::kDeferredInit:
			REX::INFO("game data is loaded");
			break;
		default:
			break;
		}
	}
}

NVSE_PLUGIN_QUERY(const NVSE::QueryInterface* a_nvse, NVSE::PluginInfo* a_info)
{
	a_info->infoVersion = NVSE::PluginInfo::kInfoVersion;
	a_info->name = NVSE_PLUGIN_NAME;
	a_info->version = NVSE_PLUGIN_VERSION;

	// the editor (GECK) is not supported by this template
	if (a_nvse->IsEditor()) {
		return false;
	}

	if (a_nvse->PackedRuntimeVersion() < NVSE::PACKED_RUNTIME_1_4_0_525) {
		return false;
	}

	return true;
}

NVSE_PLUGIN_LOAD(const NVSE::LoadInterface* a_nvse)
{
	const auto config = REX::TOML::SettingStore::GetSingleton();
	config->Init("Data/NVSE/Plugins/template.toml", "Data/NVSE/Plugins/template_custom.toml");
	config->Load();

	const auto initInfo = NVSE::InitInfo{
		.logLevel = REX::ELogLevel{ Plugin::Settings::LogLevel.GetValue() },
		.pluginName = NVSE_PLUGIN_NAME,
		.pluginVersion = NVSE_PLUGIN_VERSION
	};

	NVSE::Init(a_nvse, initInfo);

	REX::INFO("Hello World!");

	if (const auto messaging = NVSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}

	Plugin::Commands::Register(a_nvse);

	return true;
}
