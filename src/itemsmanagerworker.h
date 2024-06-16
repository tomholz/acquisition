/*
	Copyright 2014 Ilya Zhuravlev

	This file is part of Acquisition.

	Acquisition is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Acquisition is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Acquisition.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <QNetworkCookie>
#include <QNetworkRequest>
#include <QObject>

#include <queue>
#include <set>

#include "item.h"
#include "mainwindow.h"
#include "util.h"

class QNetworkAccessManager;
class QNetworkReply;
class QSignalMapper;
class QTimer;

class BuyoutManager;
class DataStore;
class RateLimiter;

enum class PoeApiMode;

struct ItemsRequest {
	int id{ -1 };
	QString endpoint;
	QNetworkRequest network_request;
	ItemLocation location;
};

struct ItemsReply {
	QNetworkReply* network_reply;
	ItemsRequest request;
};

class ItemsManagerWorker : public QObject {
	Q_OBJECT
public:
	ItemsManagerWorker(QObject* parent,
		QNetworkAccessManager& network_manager,
		BuyoutManager& buyout_manager,
		DataStore& datastore,
		RateLimiter& rate_limiter,
		std::string league,
		std::string account,
		PoeApiMode mode);
	bool isInitialized() const { return initialized_; }
	bool isUpdating() const { return updating_; };
	void UpdateRequest(TabSelection::Type type, const std::vector<ItemLocation>& locations);

signals:
	void ItemsRefreshed(const Items& items, const std::vector<ItemLocation>& tabs, bool initial_refresh);
	void StatusUpdate(ProgramState state, const QString& status);

public slots:
	void Init();
	void Update(TabSelection::Type type, const std::vector<ItemLocation>& tab_names = std::vector<ItemLocation>());

private slots:
	void OnItemClassesReceived();
	void OnItemBaseTypesReceived();
	void OnStatTranslationsReceived();

	void OnLegacyMainPageReceived();
	void OnLegacyCharacterListReceived(QNetworkReply* reply);
	void OnFirstLegacyTabReceived(QNetworkReply* reply);
	void OnLegacyTabReceived(QNetworkReply* reply, ItemLocation location);

	void OnOAuthStashListReceived(QNetworkReply* reply);
	void OnOAuthStashReceived(QNetworkReply* reply, ItemLocation location);
	void OnOAuthCharacterListReceived(QNetworkReply* reply);
	void OnOAuthCharacterReceived(QNetworkReply* reply, ItemLocation location);

private:
	void ParseItemMods();
	void RemoveUpdatingTabs(const std::set<std::string>& tab_ids);
	void RemoveUpdatingItems(const std::set<std::string>& tab_ids);
	void QueueRequest(const QString& endpoint, const QNetworkRequest& request, const ItemLocation& location);
	void FetchItems();
	void PreserveSelectedCharacter();

	void LegacyRefresh();
	QNetworkRequest MakeLegacyTabRequest(int tab_index, bool tabs = false);
	QNetworkRequest MakeLegacyCharacterRequest(const std::string& name);
	QNetworkRequest MakeLegacyPassivesRequest(const std::string& name);

	void OAuthRefresh();
	QNetworkRequest MakeOAuthStashListRequest(const std::string& league);
	QNetworkRequest MakeOAuthStashRequest(const std::string& league, const std::string& stash_id, const std::string& substash_id = "");
	QNetworkRequest MakeOAuthCharacterListRequest();
	QNetworkRequest MakeOAuthCharacterRequest(const std::string& name);

	typedef std::pair<std::string, std::string> TabSignature;
	typedef std::vector<TabSignature> TabsSignatureVector;
	TabsSignatureVector CreateTabsSignatureVector(const rapidjson::Value& tabs);

	void ParseItems(rapidjson::Value& value, ItemLocation base_location, rapidjson_allocator& alloc);
	void UpdateModList();
	bool TabsChanged(rapidjson::Document& doc, QNetworkReply* network_reply, ItemLocation& location);
	void FinishUpdate();

	QNetworkAccessManager& network_manager_;
	DataStore& datastore_;
	BuyoutManager& buyout_manager_;
	RateLimiter& rate_limiter_;

	PoeApiMode api_mode_;
	std::string league_;
	std::string account_;

	bool test_mode_;
	std::vector<ItemLocation> tabs_;
	std::queue<ItemsRequest> queue_;

	// tabs_signature_ captures <"n", "id"> from JSON tab list, used as consistency check
	TabsSignatureVector tabs_signature_;

	Items items_;
	size_t total_completed_, total_needed_;
	size_t requests_completed_, requests_needed_;

	std::set<std::string> tab_id_index_;

	volatile bool initialized_;
	volatile bool updating_;

	bool cancel_update_;
	bool updateRequest_;
	TabSelection::Type type_;
	std::vector<ItemLocation> locations_;

	int queue_id_;
	std::string selected_character_;

	int first_stash_request_index_;
	std::string first_character_request_name_;

	bool need_stash_list_;
	bool need_character_list_;

	bool has_stash_list_;
	bool has_character_list_;

	std::queue<std::string> stat_translation_queue_;
};
