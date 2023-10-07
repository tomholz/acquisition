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

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QDateTime>

#include "version_defines.h"
#include "item.h"
#include "ratelimit.h"

using RateLimit::RateLimiter;

// Holds the date and time of the current build based on __DATE__ and __TIME__ macros.
extern const QString BUILD_TIMESTAMP;

// This is BUILD_TIMESTAMP parsed into a QDateTimeObject.
extern const QDateTime BUILD_DATE;

// If TRIAL_VERSION is true (See network_defines.h) this will be the date this build
// expires, otherwise it will be an invalid QDateTime object.
extern const QDateTime EXPIRATION_DATE;

class QNetworkReply;

class DataStore;
class ItemsManager;
class BuyoutManager;
class Shop;
class CurrencyManager;

namespace RateLimit {
    class RateLimiter;
}

class Application : public QObject {
	Q_OBJECT
public:
	Application();
	~Application();
	Application(const Application&) = delete;
	Application& operator=(const Application&) = delete;
	// Should be called by login dialog after login
	void InitLogin(std::unique_ptr<QNetworkAccessManager> login_manager, const std::string &league, const std::string &email, bool mock_data = false);
	const std::string &league() const { return league_; }
	const std::string &email() const { return email_; }
	ItemsManager &items_manager() { return *items_manager_; }
	DataStore &data() const { return *data_; }
	DataStore &sensitive_data() const { return *sensitive_data_; }
	BuyoutManager &buyout_manager() const { return *buyout_manager_; }
	QNetworkAccessManager &logged_in_nm() const { return *logged_in_nm_; }
	Shop &shop() const { return *shop_; }
	CurrencyManager &currency_manager() const { return *currency_manager_; }
	RateLimiter& rate_limiter() const { return *rate_limiter_; }
public slots:
	void OnItemsRefreshed(bool initial_refresh);
    void FatalError(const QString message);
private:
	std::string league_;
	std::string email_;
	std::unique_ptr<DataStore> data_;
	// stores sensitive data that you'd rather not share, like control.poe.trade secret URL
	std::unique_ptr<DataStore> sensitive_data_;
	std::unique_ptr<BuyoutManager> buyout_manager_;
	std::unique_ptr<Shop> shop_;
	std::unique_ptr<QNetworkAccessManager> logged_in_nm_;
	std::unique_ptr<ItemsManager> items_manager_;
	std::unique_ptr<CurrencyManager> currency_manager_;
	std::unique_ptr<RateLimiter> rate_limiter_;
	void SaveDbOnNewVersion();
};
