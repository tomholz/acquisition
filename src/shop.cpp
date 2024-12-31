/*
    Copyright (C) 2014-2024 Acquisition Contributors

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

#include "shop.h"

#include <QApplication>
#include <QClipboard>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <QsLog/QsLog.h>
#include <rapidjson/document.h>

#include "datastore/datastore.h"
#include "ratelimit/ratelimitedreply.h"
#include "ratelimit/ratelimiter.h"
#include "ui/mainwindow.h"
#include "util/util.h"

#include "application.h"
#include "buyoutmanager.h"
#include "itemsmanager.h"
#include "network_info.h"
#include "replytimeout.h"

constexpr const char* kPoeEditThread = "https://www.pathofexile.com/forum/edit-thread/";
constexpr const char* kShopTemplateItems = "[items]";
constexpr int kMaxCharactersInPost = 50000;
constexpr int kSpoilerOverhead = 19; // "[spoiler][/spoiler]" length

// Use a regular expression to look for html errors.
const QRegularExpression Shop::error_regex(
    R"regex(
		# Start the match looking for any class attribute that indicates an error
		class="(?:input-error|errors)"

		# Skip over as much as possible while looking for an <li> start tag that
		# should be the start of the error message.
		.*?

		# Match the list item element and capture it's contents, because this is
		# expected to be the error message.
		<li>(.*?)</li>
	)regex",
    QRegularExpression::CaseInsensitiveOption |
    QRegularExpression::MultilineOption |
    QRegularExpression::DotMatchesEverythingOption |
    QRegularExpression::ExtendedPatternSyntaxOption);

const QRegularExpression Shop::ratelimit_regex(
    R"regex(You must wait (\d+) seconds.)regex",
    QRegularExpression::CaseInsensitiveOption);

Shop::Shop(
    QSettings& settings,
    QNetworkAccessManager& network_manager,
    RateLimiter& rate_limiter,
    DataStore& datastore,
    ItemsManager& items_manager,
    BuyoutManager& buyout_manager)
    : m_settings(settings)
    , m_network_manager(network_manager)
    , m_rate_limiter(rate_limiter)
    , m_datastore(datastore)
    , m_items_manager(items_manager)
    , m_buyout_manager(buyout_manager)
    , m_shop_data_outdated(true)
    , m_submitting(false)
    , m_indexing(false)
    , m_requests_completed(0)
{
    QLOG_TRACE() << "Shop: initializing";
    m_threads = Util::StringSplit(m_datastore.Get("shop"), ';');
    m_auto_update = m_settings.value("shop_autoupdate").toBool();
    m_shop_template = m_datastore.Get("shop_template");
    if (m_shop_template.empty()) {
        m_shop_template = kShopTemplateItems;
    };
    if (!m_settings.value("session_id").toString().isEmpty()) {
        UpdateStashIndex();
    };
}

void Shop::SetThread(const std::vector<std::string>& threads) {
    QLOG_DEBUG() << "Shop: setting thread(s) to" << Util::StringJoin(threads, ";");
    if (m_submitting) {
        return;
    };
    m_threads = threads;
    m_datastore.Set("shop", Util::StringJoin(threads, ";"));
    ExpireShopData();
    m_datastore.Set("shop_hash", "");
}

void Shop::SetAutoUpdate(bool update) {
    QLOG_DEBUG() << "Shop: setting autoupdate to" << update;
    m_auto_update = update;
    m_settings.setValue("shop_autoupdate", update);
}

void Shop::SetShopTemplate(const std::string& shop_template) {
    QLOG_DEBUG() << "Shop: setting template to" << shop_template;
    m_shop_template = shop_template;
    m_datastore.Set("shop_template", shop_template);
    ExpireShopData();
}

std::string Shop::SpoilerBuyout(Buyout& bo) {
    QLOG_TRACE() << "Shop::SpoilerBuyout() entered";
    std::string out = "";
    out += "[spoiler=\"" + bo.BuyoutTypeAsPrefix();
    if (bo.IsPriced()) {
        out += " " + QString::number(bo.value).toStdString() + " " + bo.CurrencyAsTag();
    };
    out += "\"]";
    return out;
}

void Shop::UpdateStashIndex() {

    QLOG_DEBUG() << "Shop: updating the stash index";
    m_indexing = true;
    m_tab_index.clear();

    const QString kStashItemsUrl = "https://www.pathofexile.com/character-window/get-stash-items";
    const QString account = m_settings.value("account").toString();
    const QString realm = m_settings.value("realm").toString();
    const QString league = m_settings.value("league").toString();

    QUrlQuery query;
    query.addQueryItem("accountName", account);
    query.addQueryItem("realm", realm);
    query.addQueryItem("league", league);
    query.addQueryItem("tabs", "1");
    query.addQueryItem("tabIndex", "0");

    QUrl url(kStashItemsUrl);
    url.setQuery(query);
    QNetworkRequest request(url);

    RateLimitedReply* reply = m_rate_limiter.Submit(kStashItemsUrl, request);
    connect(reply, &RateLimitedReply::complete, this, &Shop::OnStashTabIndexReceived);
}

void Shop::OnStashTabIndexReceived(QNetworkReply* reply) {

    QLOG_DEBUG() << "Shop: stash tab list received.";
    if (reply->error() != QNetworkReply::NetworkError::NoError) {
        const int status = reply->error();
        if ((status >= 200) && (status <= 299)) {
            QLOG_DEBUG() << "Shop::OnStashTabIndexReceived() network reply status" << status;
        } else {
            QLOG_ERROR() << "Shop: network error indexing stashes:" << status << reply->errorString();
            m_indexing = false;
            return;
        };
    };

    QLOG_DEBUG() << "Shop: stash tab list received";
    rapidjson::Document doc;
    QByteArray bytes = reply->readAll();
    doc.Parse(bytes.constData());
    reply->deleteLater();

    if (!doc.IsObject()) {
        QLOG_ERROR() << "Can't even fetch first legacy tab. Failed to update items.";
        m_submitting = false;
        return;
    };

    if (doc.HasMember("error")) {
        QLOG_ERROR() << "Aborting legacy update since first fetch failed due to 'error':" << Util::RapidjsonSerialize(doc["error"]);
        m_submitting = false;
        return;
    };

    if (!HasArray(doc, "tabs") || doc["tabs"].Size() == 0) {
        QLOG_ERROR() << "There are no legacy tabs, this should not happen, bailing out.";
        m_submitting = false;
        return;
    };

    auto& tabs = doc["tabs"];
    QLOG_DEBUG() << "Received legacy tabs list, there are" << tabs.Size() << "tabs";

    for (const auto& tab : tabs) {
        const int index = tab["i"].GetInt();
        const std::string uid = QString::fromUtf8(tab["id"].GetString()).first(10).toStdString();
        m_tab_index[uid] = index;
    };
    m_indexing = false;
    ExpireShopData();
    emit StashesIndexed();
}

void Shop::Update() {
    QLOG_DEBUG() << "Shop: updating shop data.";
    if (m_submitting) {
        QLOG_WARN() << "Submitting shop right now, the request to update shop data will be ignored";
        return;
    };
    if (m_indexing) {
        QLOG_WARN() << "Indexing shop right now, the request to update shop data will be ignored";
        return;
    };
    if (m_tab_index.empty()) {
        QLOG_WARN() << "Shop cannot update until stashes are indexed";
        return;
    };

    m_shop_data_outdated = false;
    m_shop_data.clear();
    std::string data = "";
    std::vector<AugmentedItem> aug_items;
    AugmentedItem tmp = AugmentedItem();
    //Get all buyouts to be able to sort them
    for (auto& item : m_items_manager.items()) {
        tmp.item = item.get();
        tmp.bo = m_buyout_manager.Get(*item);

        if (!tmp.bo.IsPostable()) {
            continue;
        };

        aug_items.push_back(tmp);
    }
    if (aug_items.size() == 0) {
        return;
    };
    std::sort(aug_items.begin(), aug_items.end());

    const std::string realm = m_settings.value("realm").toString().toStdString();
    const std::string league = m_settings.value("league").toString().toStdString();

    Buyout current_bo = aug_items[0].bo;
    data += SpoilerBuyout(current_bo);
    for (auto& aug : aug_items) {
        if (aug.bo.type != current_bo.type || aug.bo.currency != current_bo.currency || aug.bo.value != current_bo.value) {
            current_bo = aug.bo;
            data += "[/spoiler]";
            data += SpoilerBuyout(current_bo);
        };
        const ItemLocation loc = aug.item->location();
        std::string item_string;
        if (loc.get_type() == ItemLocationType::CHARACTER) {
            item_string = loc.GetForumCode(realm, league, 0);
        } else {
            const std::string uid = loc.get_tab_uniq_id();
            const auto it = m_tab_index.find(uid);
            if (it == m_tab_index.end()) {
                QLOG_ERROR() << "Cannot determine tab index for" << aug.item->PrettyName() << "in" << loc.GetHeader();
                continue;
            };
            const unsigned int ind = it->second;
            item_string = loc.GetForumCode(realm, league, ind);
        };
        const size_t n = data.size()
            + item_string.size()
            + m_shop_template.size()
            + kSpoilerOverhead
            + QStringLiteral("[/spoiler]").size();
        if (n > kMaxCharactersInPost) {
            data += "[/spoiler]";
            m_shop_data.push_back(data);
            data = SpoilerBuyout(current_bo);
            data += item_string;
        } else {
            data += item_string;
        };
    }
    if (!data.empty()) {
        m_shop_data.push_back(data);
    };

    for (size_t i = 0; i < m_shop_data.size(); ++i) {
        m_shop_data[i] = Util::StringReplace(m_shop_template, kShopTemplateItems, "[spoiler]" + m_shop_data[i] + "[/spoiler]");
    };
    m_shop_hash = Util::Md5(Util::StringJoin(m_shop_data, ";"));
}

void Shop::ExpireShopData() {
    QLOG_TRACE() << "Shop: expiring shop data";
    m_shop_data_outdated = true;
}

void Shop::SubmitShopToForum(bool force) {
    QLOG_DEBUG() << "Shop: submitting shop(s) to forums";
    if (m_submitting) {
        QLOG_WARN() << "Already submitting your shop.";
        return;
    };
    if (m_indexing) {
        QLOG_WARN() << "Still indexing tabs. Try again later.";
        return;
    };
    if (m_tab_index.empty()) {
        QLOG_ERROR() << "Please update the stash index before submitting shops";
        return;
    };
    if (m_threads.empty()) {
        QLOG_ERROR() << "Asked to update a shop with no shop ID defined.";
        QMessageBox::warning(nullptr,
            "Acquisition Shop Manager",
            "No forum threads have been set."
            "\n\n"
            "Use the Shop --> 'Forum shop thread...' menu item.");
        return;
    };
    const QString session_id = m_settings.value("session_id").toString();
    if (session_id.isEmpty()) {
        QLOG_ERROR() << "Cannot update the shop: POESESSID is not set";
        QMessageBox::warning(nullptr,
            "Acquisition Shop Manager",
            "Cannot update forum shop threads because POESESSID has not been set."
            "\n\n"
            "Use the Settings --> POESESSID --> 'show or edit session cookie' menu item.");
        return;
    };

    if (m_shop_data_outdated) {
        Update();
    };

    QLOG_INFO() << QString("Updating %1 forum shop threads").arg(m_threads.size());
    std::string previous_hash = m_datastore.Get("shop_hash");
    // Don't update the shop if it hasn't changed
    if (previous_hash == m_shop_hash && !force) {
        QLOG_TRACE() << "Shop hash has not changed. Skipping update.";
        return;
    };

    if (m_threads.size() < m_shop_data.size()) {
        QLOG_WARN() << "Need" << m_shop_data.size() - m_threads.size() << "more shops defined to fit all your items.";
    };

    m_requests_completed = 0;
    m_submitting = true;
    SubmitSingleShop();
}

std::string Shop::ShopEditUrl(size_t idx) {
    QLOG_TRACE() << "Shop::ShopEditUrl() entered";
    return kPoeEditThread + m_threads[idx];
}

void Shop::SubmitSingleShop() {
    QLOG_DEBUG() << "Shop: submitting a single shop.";
    if (m_requests_completed == m_threads.size()) {
        QLOG_INFO() << "Shop threads updated";
        emit StatusUpdate(ProgramState::Ready, "Shop threads updated");
        m_submitting = false;
        m_datastore.Set("shop_hash", m_shop_hash);
    } else {
        QLOG_INFO() << "Updating shop thread" << m_threads[m_requests_completed];
        emit StatusUpdate(ProgramState::Ready,
            QString("Sending your shops to the forum, %1/%2")
            .arg(m_requests_completed)
            .arg(m_threads.size()));
        // first, get to the edit-thread page to grab CSRF token
        QNetworkRequest request = QNetworkRequest(QUrl(ShopEditUrl(m_requests_completed).c_str()));
        request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, USER_AGENT);
        request.setRawHeader("Cache-Control", "max-age=0");
        request.setTransferTimeout(kEditThreadTimeout);
        QNetworkReply* fetched = m_network_manager.get(request);
        connect(fetched, &QNetworkReply::finished, this, &Shop::OnEditPageFinished);
    };
}

void Shop::OnEditPageFinished() {
    QLOG_TRACE() << "Shop::OnEditPageFinished() entered";
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(QObject::sender());
    const QByteArray bytes = reply->readAll();
    const std::string hash = Util::GetCsrfToken(bytes, "hash");
    if (hash.empty()) {
        if (bytes.contains("Login Required")) {
            QLOG_ERROR() << "Cannot update shop: the POESESSID is missing or invalid.";
        } else if (bytes.contains("Permission Denied")) {
            QLOG_ERROR() << "Cannot update shop: the POESESSID may be invalid or associated with another account.";
        } else {
            QLOG_ERROR() << "Cannot update shop: unable to extract CSRF token from the page. The thread ID may be invalid.";
        };
        m_submitting = false;
        return;
    };
    QLOG_TRACE() << "CSRF token found.";
    
    // now submit our edit
    // holy shit give me some html parser library please
    const std::string page(bytes.constData(), bytes.size());
    std::string title = Util::FindTextBetween(page, "<input type=\"text\" name=\"title\" id=\"title\" onkeypress=\"return&#x20;event.keyCode&#x21;&#x3D;13\" value=\"", "\">");
    if (title.empty()) {
        QLOG_ERROR() << "Cannot update shop: title is empty. Check if thread ID is valid.";
        m_submitting = false;
        reply->deleteLater();
        return;
    };

    QTimer::singleShot(500, this, [=]() { SubmitNextShop(title, hash); });
    reply->deleteLater();
}

void Shop::SubmitNextShop(const std::string title, const std::string hash)
{
    QLOG_DEBUG() << "Shop: submitting the next shop.";

    const QString content = m_requests_completed < m_shop_data.size() ? m_shop_data[m_requests_completed].c_str() : "Empty";

    QUrlQuery query;
    query.addQueryItem("title", Util::Decode(title).c_str());
    query.addQueryItem("content", content);
    query.addQueryItem("notify_owner", "0");
    query.addQueryItem("hash", hash.c_str());
    query.addQueryItem("submit", "Submit");

    QByteArray data(query.query().toUtf8());
    QNetworkRequest request((QUrl(ShopEditUrl(m_requests_completed).c_str())));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, USER_AGENT);
    request.setRawHeader("Cache-Control", "max-age=0");
    request.setTransferTimeout(kEditThreadTimeout);

    QNetworkReply* submitted = m_network_manager.post(request, data);
    connect(submitted, &QNetworkReply::finished, this,
        [=]() {
            OnShopSubmitted(query, submitted);
            submitted->deleteLater();
        });
}

void Shop::OnShopSubmitted(QUrlQuery query, QNetworkReply* reply) {
    QLOG_DEBUG() << "Shop: shop submission reply received.";
    const QByteArray bytes = reply->readAll();

    // Errors can show up in a couple different places. So far, the easiest way to identify
    // them seems to be to look for an html tag with the "class" attribute set to
    // "input-error" or "errors".
    // 
    // After this class attribute, there seems to always be an error message enclosed in
    // an list item tag, with varying differents between the class attribute and that tag.
    QRegularExpressionMatchIterator i = error_regex.globalMatch(bytes);
    if (i.hasNext()) {
        // Process one or more errors (I've never seen more than one in practice).
        int seconds = 0;
        while (i.hasNext()) {
            // We only know the error message if the list item element was found.
            const QRegularExpressionMatch error_match = i.next();
            const QString error_message = (error_match.lastCapturedIndex() > 0)
                ? QTextDocumentFragment::fromHtml(error_match.captured(1)).toPlainText()
                : "(Failed to parse the error message)";
            QLOG_ERROR() << "Error submitting shop thread:" << error_message;
            if (error_message.startsWith("Failed to find item.", Qt::CaseInsensitive)) {
                QLOG_ERROR() << "The stash index may be out of date. (Try Shop->\"Update stash index\")";
            } else if (error_message.startsWith("Security token has expired.")) {
                // This error would occur somewhat randomly before a delay was added in OnEditPageFinished.
                // With that delay, this error doesn't seem to happen any more, but we should probably
                // keep checking for it.
                if (seconds < 5) {
                    seconds = 5;
                    QLOG_TRACE() << "Setting" << seconds << "delay.";
                };
            } else if (error_message.startsWith("Rate limiting", Qt::CaseInsensitive)) {
                // Look for a rate limiting error here, because there are no headers to check for
                // like the other API calls.
                const QRegularExpressionMatch ratelimit_match = ratelimit_regex.match(error_message);
                int ratelimit_delay = ratelimit_match.captured(1).toInt();
                if (ratelimit_delay == 0) {
                    QLOG_ERROR() << "Error parsing wait time from error message.";
                    m_submitting = false;
                    return;
                };
                if (seconds < ratelimit_delay) {
                    seconds = ratelimit_delay + 1;
                    QLOG_TRACE() << "Setting" << seconds << "second delay.";
                };
            } else {
                QLOG_ERROR() << "Unknown error; the html error fragment is" << error_match.captured(0);
                QLOG_DEBUG() << "The query was:" << query.toString();
            };
        };
        if (seconds > 0) {
            // Resubmit if the errors indicate we only have to try again later.
            const int ms = seconds * 1000;
            const std::string title = query.queryItemValue("title").toStdString();
            const std::string hash = Util::GetCsrfToken(bytes, "hash");
            QLOG_WARN() << "Resubmitting shop after" << seconds << "seconds.";
            QTimer::singleShot(ms, this, [=]() { SubmitNextShop(title, hash); });
            return;
        } else {
            // Quit the update for any other error.
            m_submitting = false;
            return;
        };
    };

    // Keep legacy error-checking in place for now.
    std::string page(bytes.constData(), bytes.size());
    std::string error = Util::FindTextBetween(page, "<ul class=\"errors\"><li>", "</li></ul>");
    if (!error.empty()) {
        QLOG_ERROR() << "(DEPRECATED) Error while submitting shop to forums:" << error.c_str();
        m_submitting = false;
        return;
    };
    // This slightly different error was encountered while debugging an issue with v0.9.9-beta.1.
    // It's possible GGG has updated the forums so the previous error checking is no longer
    // relavent, but that's not certain or documented anywhere, so let's do both.
    std::string input_error = Util::FindTextBetween(page, "class=\"input-error\">", "</div>");
    if (!input_error.empty()) {
        QLOG_ERROR() << "(DEPRECATED) Input error while submitting shop to forums:" << input_error.c_str();
        m_submitting = false;
        return;
    };
    // Let's err on the side of being cautious and look for an error the above code might
    // have missed. Otherwise errors might just silently fall through the cracks.
    for (auto& substr : { "class=\"errors\"", "class=\"input-error\"" }) {
        if (page.find(substr) != std::string::npos) {
            QLOG_ERROR() << "(DEPRECATED) An error was detected but not handled while submitting shop to forums:" << substr;
            QLOG_ERROR() << page.c_str();
            m_submitting = false;
            return;
        };
    };

    ++m_requests_completed;
    SubmitSingleShop();
}

void Shop::CopyToClipboard() {
    QLOG_TRACE() << "Shop::CopyToClipboard() entered";
    if (m_shop_data_outdated) {
        QLOG_WARN() << "Shop data is outdated!";
    };
    if (m_shop_data.empty()) {
        return;
    };
    if (m_shop_data.size() > 1) {
        QLOG_WARN() << "You have more than one shop, only the first one will be copied.";
    };
    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(QString(m_shop_data[0].c_str()));
}
