/*
    Copyright 2023 Gerwaric

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

#include <QDateTime>
#include <QNetworkRequest>
#include <QObject>
#include <QTimer>

#include <deque>

#include "network_info.h"
#include "ratelimit.h"

class QNetworkAccessManager;
class QNetworkReply;

class OAuthManager;

class RateLimitManager : public QObject {
    Q_OBJECT

public:
    RateLimitManager(QObject* parent,
        QNetworkAccessManager& network_manager,
        OAuthManager& oauth_manager,
        POE_API mode);

    // Move a request into to this manager's queue.
    void QueueRequest(
        const QString& endpoint,
        const QNetworkRequest request,
        RateLimit::RateLimitedReply* reply);

    void Update(QNetworkReply* reply);

    const RateLimit::Policy& policy();

    const QDateTime& next_send() const { return next_send_; };

    bool isActive() const { return (active_request_ != nullptr); };

    POE_API mode() const { return mode_; };

signals:
    // Emitted when a network request is ready to go.
    void RequestReady(RateLimitManager* manager, QNetworkRequest request, POE_API mode);

    // Emitted when the underlying policy has been updated.
    void PolicyUpdated(const RateLimit::Policy& policy);

public slots:
    // Called when a reply has been received. Checks for errors. Updates the
    // rate limit policy if one was received. Puts the response in the
    // dispatch queue for callbacks. Checks to see if another request is
    // waiting to be activated.
    void ReceiveReply();

private slots:

    // Sends the currently active request and connects it to ReceiveReply().
    void SendRequest();

private:

    QNetworkAccessManager& network_manager_;
    OAuthManager& oauth_manager_;

    // Determines which API this rate limit manager will be used for.
    const POE_API mode_;

    // Represents a single rate-limited request.
    struct RateLimitedRequest {

        // Construct a new rate-limited request.
        RateLimitedRequest(const QString& endpoint_, const QNetworkRequest& network_request_, RateLimit::RateLimitedReply* reply_) :
            id(++request_count),
            endpoint(endpoint_),
            network_request(network_request_),
            reply(reply_) {}

        // Unique identified for each request, even through different requests can be
        // routed to different policy managers based on different endpoints.
        const unsigned long id;

        // A copy of this request's API endpoint, if any.
        const QString endpoint;

        // A copy of the network request that's going to be sent.
        QNetworkRequest network_request;

        std::unique_ptr<RateLimit::RateLimitedReply> reply;

    private:

        // Total number of requests that have every been constructed.
        static unsigned long request_count;
    };

    // Called right after active_request is loaded with a new request. This
    // will determine when that request can be sent and setup the active
    // request timer to send that request after a delay.
    void ActivateRequest();

    void FatalError(const QString& message);

    // Resends the active request after a delay due to a violation.
    //void ResendAfterViolation();

    // Used to send requests after a delay.
    QTimer activation_timer_;

    // When a reply is recieved and the policy state has been updated or a 
    // rate violation has been detected, the next possible send time is calculated
    // and stored here.
    QDateTime next_send_;

    // Store the time of the last send for this policy, just so we can have an
    // extra check to make sure we don't flood GGG with requests.
    QDateTime last_send_;

    // Keep a unique_ptr to the policy associated with this manager,
    // which will be updated whenever a reply with the X-Rate-Limit-Policy
    // header is received.
    std::unique_ptr<RateLimit::Policy> policy_;

    // The active request
    std::unique_ptr<RateLimitedRequest> active_request_;

    // Requests that are waiting to be activated.
    std::deque<std::unique_ptr<RateLimitedRequest>> queued_requests_;

    // We use a history of the received reply times so that we can calculate
    // when the next safe send time will be. This allows us to calculate the
    // least delay necessary to stay compliant.
    //
    // A circular buffer is used because it's fast to access, and the number
    // of items we have to store only changes when a rate limit policy
    // changes, which should not happen regularly, but we handle that case, too.
    RateLimit::RequestHistory history_;
};
