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

#include "logindialog.h"
#include "ui_logindialog.h"

#include <QDesktopServices>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkProxyFactory>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>
#include <iostream>
#include "QsLog.h"

#include "application.h"
#include "filesystem.h"
#include "mainwindow.h"
#include "network_info.h"
#include "replytimeout.h"
#include "selfdestructingreply.h"
#include "util.h"
#include "updatechecker.h"
#include "oauth.h"
#include "version_defines.h"

const char* POE_LEAGUE_LIST_URL = "https://api.pathofexile.com/leagues?type=main&compact=1";
const char* POE_LOGIN_URL = "https://www.pathofexile.com/login";
const char* POE_MAIN_PAGE = "https://www.pathofexile.com/";
const char* POE_MY_ACCOUNT = "https://www.pathofexile.com/my-account";
const char* POE_LOGIN_CHECK_URL = POE_MY_ACCOUNT;
const char* POE_COOKIE_NAME = "POESESSID";

const char* LOGIN_CHECK_ERROR = "Failed to log in. Try copying your session ID again, or try OAuth";

const char* OAUTH_TAB = "oauthTab";
const char* SESSIONID_TAB = "sessionIdTab";

/**
 * Possible login flows:

 * OAuth
	=> Point browser to OAuth login page
	=> OnSteamCookieReceived() -> LoginWithCookie()
	=> Retrieve POE_LOGIN_CHECK_URL
	=> LoggedInCheck()
	=> Retrieve /my-account to get account name
	=> OnMainPageFinished()
	=> done

  * Session ID
	=> LoginWithCookie()
	=> Retrieve POE_LOGIN_CHECK_URL
	=> LoggedInCheck()
	=> Retrieve /my-account to get account name
	=> OnMainPageFinished()
	=> done
 */

LoginDialog::LoginDialog(std::unique_ptr<Application> app) :
	app_(std::move(app)),
	ui(new Ui::LoginDialog),
	mw(0),
	asked_to_update_(false)
{
	ui->setupUi(this);
	ui->errorLabel->hide();
	ui->errorLabel->setStyleSheet("QLabel { color : red; }");
    setWindowTitle(QString("Login [") + APP_VERSION_STRING + "]");
#if defined(Q_OS_LINUX)
	setWindowIcon(QIcon(":/icons/assets/icon.svg"));
#endif
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	settings_path_ = Filesystem::UserDir() + "/settings.ini";
	LoadSettings();

	QLOG_DEBUG() << "Supports SSL: " << QSslSocket::supportsSsl();
	QLOG_DEBUG() << "SSL Library Build Version: " << QSslSocket::sslLibraryBuildVersionString();
	QLOG_DEBUG() << "SSL Library Version: " << QSslSocket::sslLibraryVersionString();

	connect(ui->proxyCheckBox, SIGNAL(clicked(bool)), this, SLOT(OnProxyCheckBoxClicked(bool)));
	connect(ui->loginButton, SIGNAL(clicked()), this, SLOT(OnLoginButtonClicked()));
	connect(&app_->update_checker(), &UpdateChecker::UpdateAvailable, this, [&]() {
		// Only annoy the user once at the login dialog window, even if it's opened for more than an hour
		if (asked_to_update_)
			return;
		asked_to_update_ = true;
		UpdateChecker::AskUserToUpdate(this);
		});

	QNetworkRequest leagues_request = QNetworkRequest(QUrl(QString(POE_LEAGUE_LIST_URL)));
	leagues_request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, USER_AGENT);
	QNetworkReply* leagues_reply = app_->network_manager().get(leagues_request);

	connect(leagues_reply, &QNetworkReply::errorOccurred, this, &LoginDialog::errorOccurred);
	connect(leagues_reply, &QNetworkReply::sslErrors, this, &LoginDialog::sslErrorOccurred);
	connect(leagues_reply, &QNetworkReply::finished, this, &LoginDialog::OnLeaguesRequestFinished);
	new QReplyTimeout(leagues_reply, kPoeApiTimeout);
}

void LoginDialog::errorOccurred() {
	QLOG_ERROR() << "League List errorOccured";
}

void LoginDialog::sslErrorOccurred() {
	QLOG_ERROR() << "League List sslErrorOccured";
}

void LoginDialog::OnLoginButtonClicked() {
	ui->loginButton->setEnabled(false);
	ui->loginButton->setText("Logging in...");

	const QString tab_name = ui->loginTabs->currentWidget()->objectName();

	if (tab_name == OAUTH_TAB) {
		LoginWithOAuth();
	} else if (tab_name == SESSIONID_TAB) {
		LoginWithCookie(ui->sessionIDLineEdit->text());
	} else {
		QLOG_ERROR() << "Invalid login tab name:" << tab_name;
	};
}

void LoginDialog::LeaguesApiError(const QString& error, const QByteArray& reply) {
	DisplayError("Leagues API returned malformed data: " + error, true);
	QLOG_ERROR() << "Leagues API says: " << reply;
}

void LoginDialog::OnLeaguesRequestFinished() {
	SelfDestructingReply reply(qobject_cast<QNetworkReply*>(QObject::sender()));
	QByteArray bytes = reply->readAll();

	if (reply->error())
		return LeaguesApiError(reply->errorString(), bytes);

	// Trial builds come with an expiration date. Prevent login of expired builds.
	if (TRIAL_VERSION) {
		// Make sure the expiration date is valid.
		const QString expiration = EXPIRATION_DATE.toString();
		if (EXPIRATION_DATE.isValid() == false) {
			QLOG_ERROR() << "This is a trial build, but the expiration date is invalid";
			DisplayError("This is a trial build, but the expiration date is invalid");
			ui->loginButton->setEnabled(false);
			return;
		};
		// Make sure the reply header date is valid.
		const QByteArray reply_timestamp = Util::FixTimezone(reply->rawHeader("Date"));
		const QDateTime reply_date = QDateTime::fromString(reply_timestamp, Qt::RFC2822Date);
		if (reply_date.isValid() == false) {
			QLOG_ERROR() << "Cannot determine the current date of an expiring trial build.";
			DisplayError("Cannot determine the current date of an expiring trial build");
			ui->loginButton->setEnabled(false);
			return;
		};
		// Make sure the build hasn't expired.
		if (EXPIRATION_DATE < reply_date) {
			QLOG_ERROR() << "This build expired on" << expiration;
			DisplayError("This build expired on " + expiration);
			ui->loginButton->setEnabled(false);
			return;
		};
		// Warn the user that this build will expire.
		QLOG_WARN() << "This build will expire on" << expiration;
		DisplayError("This build will expire on " + expiration);
	};

	rapidjson::Document doc;
	doc.Parse(bytes.constData());

	if (doc.HasParseError() || !doc.IsArray())
		return LeaguesApiError("Failed to parse the document", bytes);

	ui->leagueComboBox->clear();
	for (auto& league : doc) {
		if (!league.IsObject())
			return LeaguesApiError("Object expected", bytes);
		if (!league.HasMember("id"))
			return LeaguesApiError("Missing league 'id'", bytes);
		if (!league["id"].IsString())
			return LeaguesApiError("String expected", bytes);
		ui->leagueComboBox->addItem(league["id"].GetString());
	}
	ui->leagueComboBox->setEnabled(true);

	if (saved_league_.size() > 0)
		ui->leagueComboBox->setCurrentText(saved_league_);
}

// All characters except + should be handled by QUrlQuery
// See https://doc.qt.io/qt-6/qurlquery.html#encoding
static QString EncodeSpecialCharacters(QString s) {
	s.replace("+", "%2b");
	return s;
}

void LoginDialog::FinishLogin(QNetworkReply* reply) {
	QList<QNetworkCookie> cookies = reply->manager()->cookieJar()->cookiesForUrl(QUrl(POE_MAIN_PAGE));
	for (QNetworkCookie& cookie : cookies)
		if (QString(cookie.name()) == POE_COOKIE_NAME)
			session_id_ = cookie.value();

	// we need one more request to get account name
	QNetworkRequest main_page_request = QNetworkRequest(QUrl(POE_MY_ACCOUNT));
	main_page_request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, USER_AGENT);
	QNetworkReply* main_page = app_->network_manager().get(main_page_request);
	connect(main_page, SIGNAL(finished()), this, SLOT(OnMainPageFinished()));
}

void LoginDialog::OnLoggedIn() {
	QNetworkReply* reply = qobject_cast<QNetworkReply*>(QObject::sender());
	QByteArray bytes = reply->readAll();
	int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status != 302) {
		DisplayError(LOGIN_CHECK_ERROR);
		return;
	}

	FinishLogin(reply);
}

// Need a separate check since it's just the /login URL that's filtered
void LoginDialog::LoggedInCheck() {
	QNetworkReply* reply = qobject_cast<QNetworkReply*>(QObject::sender());
	QByteArray bytes = reply->readAll();
	int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	switch (status) {
	case 302:
		DisplayError(LOGIN_CHECK_ERROR);
		return;
	case 401:
		DisplayError(LOGIN_CHECK_ERROR);
		return;
	}
	FinishLogin(reply);
}

void LoginDialog::LoginWithOAuth() {
	connect(&app_->oauth_manager(), &OAuthManager::accessGranted,
		this, &LoginDialog::OnOAuthAccessGranted);
	app_->oauth_manager().requestAccess();
}

void LoginDialog::OnOAuthAccessGranted(const AccessToken& token) {
	const QString account = token.username;
	std::string league(ui->leagueComboBox->currentText().toStdString());
	app_->InitLogin(league, account.toStdString());
	mw = new MainWindow(std::move(app_));
	mw->setWindowTitle(
		QString("Acquisition [%1] - %2 [%3]")
        .arg(APP_VERSION_STRING)
		.arg(league.c_str())
		.arg(account));
	mw->show();
	close();
}

void LoginDialog::LoginWithCookie(const QString& cookie) {
	QNetworkCookie poeCookie(POE_COOKIE_NAME, cookie.toUtf8());
	poeCookie.setPath("/");
	poeCookie.setDomain(".pathofexile.com");

	app_->network_manager().cookieJar()->insertCookie(poeCookie);

	QNetworkRequest login_page_request = QNetworkRequest(QUrl(POE_LOGIN_CHECK_URL));
	login_page_request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, USER_AGENT);
	QNetworkReply* login_page = app_->network_manager().get(login_page_request);
	connect(login_page, SIGNAL(finished()), this, SLOT(LoggedInCheck()));
}

void LoginDialog::OnMainPageFinished() {
	QNetworkReply* reply = qobject_cast<QNetworkReply*>(QObject::sender());
	QString html(reply->readAll());
	QRegularExpression regexp("/account/view-profile/(.*?)\"");
	QRegularExpressionMatch match = regexp.match(html, 0);
	if (match.hasMatch() == false) {
		DisplayError("Failed to find account name.");
		return;
	}
	QString account = match.captured(1);
	QLOG_DEBUG() << "Logged in as:" << account;

	std::string league(ui->leagueComboBox->currentText().toStdString());
	app_->InitLogin(league, account.toStdString());
	mw = new MainWindow(std::move(app_));
	mw->setWindowTitle(
		QString("Acquisition [%1] - %2 [%3]")
        .arg(APP_VERSION_STRING)
		.arg(league.c_str())
		.arg(account));
	mw->show();
	close();
}

void LoginDialog::OnProxyCheckBoxClicked(bool checked) {
	QNetworkProxyFactory::setUseSystemConfiguration(checked);
}

void LoginDialog::LoadSettings() {
	QSettings settings(settings_path_.c_str(), QSettings::IniFormat);
	session_id_ = settings.value("session_id", "").toString();
	ui->sessionIDLineEdit->setText(session_id_);
	ui->rembmeCheckBox->setChecked(settings.value("remember_me_checked").toBool());
	ui->proxyCheckBox->setChecked(settings.value("use_system_proxy_checked").toBool());

	if (ui->rembmeCheckBox->isChecked()) {
		for (auto i = 0; i < ui->loginTabs->count(); ++i) {
			if (ui->loginTabs->widget(i)->objectName() == SESSIONID_TAB) {
				ui->loginTabs->setCurrentIndex(i);
				break;
			};
		};
	};

	saved_league_ = settings.value("league", "").toString();
	if (saved_league_.size() > 0)
		ui->leagueComboBox->setCurrentText(saved_league_);

	QNetworkProxyFactory::setUseSystemConfiguration(ui->proxyCheckBox->isChecked());
}

void LoginDialog::SaveSettings() {
	QSettings settings(settings_path_.c_str(), QSettings::IniFormat);
	if (ui->rembmeCheckBox->isChecked()) {
		settings.setValue("session_id", session_id_);
		settings.setValue("league", ui->leagueComboBox->currentText());
	} else {
		settings.setValue("session_id", "");
		settings.setValue("league", "");
	}
	settings.setValue("remember_me_checked", ui->rembmeCheckBox->isChecked() && !session_id_.isEmpty());
	settings.setValue("use_system_proxy_checked", ui->proxyCheckBox->isChecked());
}

void LoginDialog::DisplayError(const QString& error, bool disable_login) {
	ui->errorLabel->setText(error);
	ui->errorLabel->show();
	ui->loginButton->setEnabled(!disable_login);
	ui->loginButton->setText("Login");
}

LoginDialog::~LoginDialog() {
	SaveSettings();
	delete ui;

	if (mw)
		delete mw;
}

bool LoginDialog::event(QEvent* e) {
	if (e->type() == QEvent::LayoutRequest)
		setFixedSize(sizeHint());
	return QDialog::event(e);
}
