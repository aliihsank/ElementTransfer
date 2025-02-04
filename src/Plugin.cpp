#include "API/ARK/Ark.h"

#include <fstream>
#include <json.hpp>

#include <mysql+++.h>

using namespace std;

using json = nlohmann::json;

json config;

bool enableDebugging;

daotk::mysql::connection db_;

FString elementBP = "Blueprint'/Game/PrimalEarth/CoreBlueprints/Resources/PrimalItemResource_Element.PrimalItemResource_Element'";
FString elementShardBP = "Blueprint'/Game/PrimalEarth/CoreBlueprints/Resources/PrimalItemResource_ElementShard.PrimalItemResource_ElementShard'";

FString GetItemBlueprint(UPrimalItem* item)
{
	if (item != nullptr)
	{
		FString path_name;
		item->ClassField()->GetDefaultObject(true)->GetFullName(&path_name, nullptr);

		if (int find_index = 0; path_name.FindChar(' ', find_index))
		{
			path_name = "Blueprint'" + path_name.Mid(find_index + 1,
				path_name.Len() - (find_index + (path_name.EndsWith(
					"_C", ESearchCase::
					CaseSensitive)
					? 3
					: 1))) + "'";
			return path_name.Replace(L"Default__", L"", ESearchCase::CaseSensitive);
		}
	}

	return FString("");
}

void Upload(AShooterPlayerController* player_controller, FString* message, EChatSendMode::Type /*unused*/)
{
	try 
	{
		UPrimalInventoryComponent* inventory = player_controller->GetPlayerCharacter()->MyInventoryComponentField();
		if (inventory == nullptr)
		{
			return;
		}

		int element_count = 0;
		int element_shard_count = 0;

		TArray<UPrimalItem*> items = inventory->InventoryItemsField();
		for (UPrimalItem* item : items)
		{
			if (item->ClassField() && item->bAllowRemovalFromInventory()() && !item->bIsEngram()())
			{
				const FString item_bp = GetItemBlueprint(item);

				if (item_bp == elementBP)
				{
					element_count += item->GetItemQuantity();

					inventory->RemoveItem(&item->ItemIDField(), false, false, true, true);
				}

				if (item_bp == elementShardBP)
				{
					element_shard_count += item->GetItemQuantity();

					inventory->RemoveItem(&item->ItemIDField(), false, false, true, true);
				}
			}
		}

		if (element_count == 0 && element_shard_count == 0) 
		{
			auto* shooter_controller = static_cast<AShooterPlayerController*>(player_controller);

			ArkApi::GetApiUtils().SendServerMessage(shooter_controller, FColorList::Red, "No items to upload!");

			return;
		}

		const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(player_controller);

		if (enableDebugging) {
			Log::GetLog()->info(fmt::format("Searching existing transfer data for steam_id: {}", steam_id));

			Log::GetLog()->info(fmt::format("Element: {}, Shard: {}", element_count, element_shard_count));
		}

		int id = 0;
		auto res = db_.query(fmt::format("SELECT Id FROM ElementTransferPlayers WHERE SteamId = {};", steam_id));

		if (res)
		{
			res.fetch(id);
		}

		if (id > 0)
		{
			if (enableDebugging) {
				Log::GetLog()->info("Updating transfer data...");
			}

			db_.query(fmt::format("UPDATE ElementTransferPlayers SET ElementCount = ElementCount + {}, ShardCount = ShardCount + {} WHERE SteamId = {};", element_count, element_shard_count, steam_id));
		}
		else
		{
			if (enableDebugging) {
				Log::GetLog()->info("Adding transfer data...");
			}

			db_.query(fmt::format("INSERT INTO ElementTransferPlayers(SteamId, ElementCount, ShardCount) VALUES ({}, {}, {});", steam_id, element_count, element_shard_count));
		}
	}
	catch (const std::exception& error)
	{
		Log::GetLog()->error(error.what());
	}
}

void Download(AShooterPlayerController* player_controller, FString* message, EChatSendMode::Type /*unused*/)
{
	try
	{
		if (ArkApi::IApiUtils::IsPlayerDead(player_controller))
		{
			return;
		}

		const uint64 steam_id = ArkApi::IApiUtils::GetSteamIdFromController(player_controller);

		int element_count = 0;
		int element_shard_count = 0;
		auto elementTransfer = db_.query(fmt::format("SELECT ElementCount, ShardCount FROM ElementTransferPlayers WHERE SteamId = {};", steam_id));
		if (elementTransfer.is_empty()) 
		{
			auto* shooter_controller = static_cast<AShooterPlayerController*>(player_controller);

			ArkApi::GetApiUtils().SendServerMessage(shooter_controller, FColorList::Red, "No items to download!");

			return;
		}
		
		elementTransfer.fetch(element_count, element_shard_count);

		db_.query(fmt::format("DELETE FROM ElementTransferPlayers WHERE SteamId = {};", steam_id));

		if (element_count > 0) {
			TArray<UPrimalItem*> out_items;
			player_controller->GiveItem(&out_items, &elementBP, element_count, 0, false, false, 0);
		}

		if (element_shard_count > 0) {
			TArray<UPrimalItem*> out_items;
			player_controller->GiveItem(&out_items, &elementShardBP, element_shard_count, 0, false, false, 0);
		}
	}
	catch (const std::exception& error)
	{
		Log::GetLog()->error(error.what());
	}
}

void ReadConfig()
{
	const std::string config_path = ArkApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/ElementTransfer/configs/config.json";

	std::ifstream f(config_path);
	config = json::parse(f);
}

// Called by ArkServerApi when the plugin is loaded, do pre-"server ready" initialization here
extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init(PROJECT_NAME);

	try
	{
		ReadConfig();
	}
	catch (const std::exception& error)
	{
		Log::GetLog()->error(error.what());
		throw;
	}

	try
	{
		enableDebugging = config["EnableDebugging"];
		const bool enable = config["Enable"];
		const auto& mysql_conf = config["Mysql"];

		if (enable)
		{
			try
			{
				daotk::mysql::connect_options options;
				options.server = move(mysql_conf.value("MysqlHost", ""));
				options.username = move(mysql_conf.value("MysqlUser", ""));
				options.password = move(mysql_conf.value("MysqlPass", ""));
				options.dbname = move(mysql_conf.value("MysqlDB", ""));
				options.autoreconnect = true;
				options.timeout = 30;
				options.port = 3306;

				bool result = db_.open(options);
				if (!result)
				{
					Log::GetLog()->critical("Failed to open database connection check your settings!");
				}

				db_.query("CREATE TABLE IF NOT EXISTS ElementTransferPlayers ("
					"Id INT NOT NULL AUTO_INCREMENT,"
					"SteamId BIGINT(20) UNSIGNED NOT NULL DEFAULT 0,"
					"ElementCount INT DEFAULT 0,"
					"ShardCount INT DEFAULT 0,"
					"PRIMARY KEY(Id),"
					"UNIQUE INDEX SteamId_UNIQUE (SteamId ASC));");
			}
			catch (const std::exception& exception)
			{
				Log::GetLog()->error("({} {}) Unexpected DB error {}", __FILE__, __FUNCTION__, exception.what());
			}
		}
		else 
		{
			Log::GetLog()->info("ElementTransfer Plugin is not enabled!");
		}

		auto& commands = ArkApi::GetCommands();

		commands.AddChatCommand("/upload", &Upload);
		commands.AddChatCommand("/download", &Download);
	}
	catch (const std::exception& error)
	{
		Log::GetLog()->error(error.what());
		throw;
	}
}

// Called by ArkServerApi when the plugin is unloaded, do cleanup here
extern "C" __declspec(dllexport) void Plugin_Unload()
{
	ArkApi::GetCommands().RemoveChatCommand("/upload");
	ArkApi::GetCommands().RemoveChatCommand("/download");
}