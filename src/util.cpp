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

#include "util.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QString>
#include <QStringList>
#include <QLineEdit>
#include <QLabel>
#include <QFontMetrics>
#include <QMetaEnum>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrlQuery>
#include <QPainter>

#include <sstream>
#include <iomanip>
#include <cmath>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/prettywriter.h"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string.hpp>

#include "currency.h"

std::string Util::Md5(const std::string& value) {
    QString hash = QString(QCryptographicHash::hash(value.c_str(), QCryptographicHash::Md5).toHex());
    return hash.toUtf8().constData();
}

double Util::AverageDamage(const std::string& s) {
    size_t x = s.find("-");
    if (x == std::string::npos)
        return 0;
    return (std::stod(s.substr(0, x)) + std::stod(s.substr(x + 1))) / 2;
}

void Util::PopulateBuyoutTypeComboBox(QComboBox* combobox) {
    combobox->addItems(QStringList({ "[Ignore]", "Buyout", "Fixed price", "Current Offer", "No price", "[Inherit]" }));
    combobox->setCurrentIndex(5);
}

void Util::PopulateBuyoutCurrencyComboBox(QComboBox* combobox) {
    for (auto type : Currency::Types())
        combobox->addItem(QString(Currency(type).AsString().c_str()));
}

constexpr std::array width_strings = {
    "max#",
    "Map Tier",
    "R##",
    "Defense",
    "Master-crafted"
};

int Util::TextWidth(TextWidthId id) {
    static bool calculated = false;
    static std::vector<int> result;

    if (!calculated) {
        calculated = true;
        result.resize(width_strings.size());
        QLineEdit textbox;
        QFontMetrics fm(textbox.fontMetrics());
        for (size_t i = 0; i < width_strings.size(); ++i)
            result[i] = fm.horizontalAdvance(width_strings[i]);
    }
    return result[static_cast<int>(id)];
}

void Util::ParseJson(QNetworkReply* reply, rapidjson::Document* doc) {
    QByteArray bytes = reply->readAll();
    doc->Parse(bytes.constData());
}

std::string Util::GetCsrfToken(const QByteArray& page, const std::string& name) {
    // As of October 2023, the CSRF token can appear in one of two ways:
    //  name="hash" value="..."
    //	or
    //	name="hash" class="input-error" value="..."
    static const QString expr = QString(
        R"regex(
			name="%1"
			\s+
			(?:
				class=".*?"
				\s+
			)?
			value="(.*?)"
		)regex").arg(QString::fromStdString(name));
    static const QRegularExpression re(expr,
        QRegularExpression::CaseInsensitiveOption |
        QRegularExpression::MultilineOption |
        QRegularExpression::DotMatchesEverythingOption |
        QRegularExpression::ExtendedPatternSyntaxOption);
    const QRegularExpressionMatch match = re.match(page);
    return match.captured(1).toStdString();
}

std::string Util::FindTextBetween(const std::string& page, const std::string& left, const std::string& right) {
    size_t first = page.find(left);
    size_t last = page.find(right, first);
    if (first == std::string::npos || last == std::string::npos || first > last)
        return "";
    return page.substr(first + left.size(), last - first - left.size());
}

std::string Util::RapidjsonSerialize(const rapidjson::Value& val) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    val.Accept(writer);
    return buffer.GetString();
}

std::string Util::RapidjsonPretty(const rapidjson::Value& val) {
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    val.Accept(writer);
    return buffer.GetString();
}

void Util::RapidjsonAddString(rapidjson::Value* object, const char* const name, const std::string& value, rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& alloc) {
    rapidjson::Value rjson_name;
    rjson_name.SetString(name, rapidjson::SizeType(strlen(name)), alloc);
    rapidjson::Value rjson_val;
    rjson_val.SetString(value.c_str(), rapidjson::SizeType(value.size()), alloc);
    object->AddMember(rjson_name, rjson_val, alloc);
}

void Util::RapidjsonAddConstString(rapidjson::Value* object, const char* const name, const std::string& value, rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& alloc) {
    rapidjson::Value rjson_name;
    rjson_name.SetString(name, rapidjson::SizeType(strlen(name)));
    rapidjson::Value rjson_val;
    rjson_val.SetString(value.c_str(), rapidjson::SizeType(value.size()));
    object->AddMember(rjson_name, rjson_val, alloc);
}

void Util::RapidjsonAddInt64(rapidjson::Value* object, const char* const name, qint64 value, rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& alloc) {
    rapidjson::Value rjson_name;
    rjson_name.SetString(name, rapidjson::SizeType(strlen(name)));
    rapidjson::Value rjson_val;
    rjson_val.SetInt64(value);
    object->AddMember(rjson_name, rjson_val, alloc);
}

std::string Util::StringReplace(const std::string& haystack, const std::string& needle, const std::string& replace) {
    std::string out = haystack;
    for (size_t pos = 0; ; pos += replace.length()) {
        pos = out.find(needle, pos);
        if (pos == std::string::npos)
            break;
        out.erase(pos, needle.length());
        out.insert(pos, replace);
    }
    return out;
}

std::string Util::StringJoin(const std::vector<std::string>& arr, const std::string& separator) {
    return boost::join(arr, separator);
}

std::vector<std::string> Util::StringSplit(const std::string& str, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

bool Util::MatchMod(const char* match, const char* mod, double* output) {
    double result = 0.0;
    auto pmatch = match;
    auto pmod = mod;
    int cnt = 0;

    while (*pmatch && *pmod) {
        if (*pmatch == '#') {
            ++cnt;
            auto prev = pmod;
            while ((*pmod >= '0' && *pmod <= '9') || *pmod == '.')
                ++pmod;
            result += std::strtod(prev, NULL);
            ++pmatch;
        } else if (*pmatch == *pmod) {
            ++pmatch;
            ++pmod;
        } else {
            return false;
        }
    }
    *output = result / cnt;
    return !*pmatch && !*pmod;
}

std::string Util::Capitalise(const std::string& str) {
    std::string capitalised = str;
    capitalised[0] = static_cast<std::string::value_type>(toupper(capitalised[0]));   //  Set the first character to upper case
    return capitalised;
}

std::string Util::TimeAgoInWords(const QDateTime buyout_time) {
    QDateTime current_date = QDateTime::currentDateTime();
    qint64 secs = buyout_time.secsTo(current_date);
    qint64 days = secs / 60 / 60 / 24;
    qint64 hours = (secs / 60 / 60) % 24;
    qint64 minutes = (secs / 60) % 60;

    // YEARS
    if (days > 365) {
        int years = (days / 365);
        if (days % 365 != 0)
            years++;
        return QString("%1 %2 ago").arg(years).arg(years == 1 ? "year" : "years").toStdString();
    }
    // MONTHS
    if (days > 30) {
        int months = (days / 365);
        if (days % 30 != 0)
            months++;
        return QString("%1 %2 ago").arg(months).arg(months == 1 ? "month" : "months").toStdString();
        // DAYS
    } else if (days > 0) {
        return QString("%1 %2 ago").arg(days).arg(days == 1 ? "day" : "days").toStdString();
        // HOURS
    } else if (hours > 0) {
        return QString("%1 %2 ago").arg(hours).arg(hours == 1 ? "hour" : "hours").toStdString();
        //MINUTES
    } else if (minutes > 0) {
        return QString("%1 %2 ago").arg(minutes).arg(minutes == 1 ? "minute" : "minutes").toStdString();
        // SECONDS
    } else if (secs > 5) {
        return QString("%1 %2 ago").arg(secs).arg("seconds").toStdString();
    } else if (secs < 5) {
        return QString("just now").toStdString();
    } else {
        return "";
    }
}

std::string Util::Decode(const std::string& entity) {
    QTextDocument text;
    text.setHtml(entity.c_str());
    return text.toPlainText().toStdString();
}

QUrlQuery Util::EncodeQueryItems(const std::list<std::pair<QString, QString>>& items) {
    // https://github.com/owncloud/client/issues/9203
    QUrlQuery result;
    for (const auto& item : items) {
        const QString key = QUrl::toPercentEncoding(item.first);
        const QString value = QUrl::toPercentEncoding(item.second);
        result.addQueryItem(key, value);
    };
    return result;
}

QColor Util::recommendedForegroundTextColor(const QColor& backgroundColor) {
    float R = (float)backgroundColor.red() / 255.0f;
    float G = (float)backgroundColor.green() / 255.0f;
    float B = (float)backgroundColor.blue() / 255.0f;

    const float gamma = 2.2f;
    float L = 0.2126f * pow(R, gamma)
        + 0.7152f * pow(G, gamma)
        + 0.0722f * pow(B, gamma);

    return (L > 0.5f) ? QColor(QColorConstants::Black) : QColor(QColorConstants::White);
}

std::string Util::hexStr(const uint8_t* data, int len)
{
    std::stringstream ss;
    ss << std::hex;

    for (int i(0); i < len; ++i)
        ss << std::setw(2) << std::setfill('0') << (int)data[i];

    std::string temp = ss.str();
    boost::to_upper(temp);

    return temp;
}

// Obsolete timezones are allowed by RFC2822, but they aren't parsed by
// QT 6.5.3 so we have to fix them manually.
QByteArray Util::FixTimezone(const QByteArray& rfc2822_date) {
    const std::vector<std::pair<QByteArray, QByteArray>> OBSOLETE_ZONES = {
        {"GMT", "+0000"},
        {"UT" , "+0000"},
        {"EST", "-0005"},
        {"EDT", "-0004"},
        {"CST", "-0006"},
        {"CDT", "-0005"},
        {"MST", "-0007"},
        {"MDT", "-0006"},
        {"PST", "-0008"},
        {"PDT", "-0007"}
    };
    for (auto& pair : OBSOLETE_ZONES) {
        const QByteArray& zone = pair.first;
        const QByteArray& offset = pair.second;
        if (rfc2822_date.endsWith(zone)) {
            const int k = rfc2822_date.length() - zone.length();
            return rfc2822_date.left(k) + offset;
        };
    };
    return rfc2822_date;
}

QDebug& operator<<(QDebug& os, const RefreshReason::Type& obj)
{
    const QMetaObject* meta = &RefreshReason::staticMetaObject;
    os << meta->enumerator(meta->indexOfEnumerator("Type")).key(obj);
    return os;
}

QDebug& operator<<(QDebug& os, const TabSelection::Type& obj)
{
    const QMetaObject* meta = &TabSelection::staticMetaObject;
    os << meta->enumerator(meta->indexOfEnumerator("Type")).key(obj);
    return os;
}

QDebug& operator<<(QDebug& os, const QsLogging::Level& obj) {
    switch (obj) {
    case QsLogging::Level::TraceLevel: return os << "TRACE";
    case QsLogging::Level::DebugLevel: return os << "DEBUG";
    case QsLogging::Level::InfoLevel: return os << "INFO";
    case QsLogging::Level::WarnLevel: return os << "WARN";
    case QsLogging::Level::ErrorLevel: return os << "ERROR";
    case QsLogging::Level::FatalLevel: return os << "FATAL";
    case QsLogging::Level::OffLevel: return os << "OFF";
    default: return os << "None (log level is invalid)";
    };
}


std::string Util::ConvertAsciiToUtf(const std::string& asciiString) {
    std::string utfString;

    for (size_t i = 0; i < asciiString.size(); ++i) {
        // Check if the character is an escape character
        if (asciiString[i] == '\\' && i + 1 < asciiString.size() && asciiString[i + 1] == 'u') {
            // Fetch the next four characters after '\u'
            std::string unicodeStr = asciiString.substr(i + 2, 4);

            // Convert the hexadecimal Unicode representation to integer
            unsigned int unicodeValue = std::stoi(unicodeStr, nullptr, 16);

            // Append the Unicode character to the UTF-8 string
            utfString.push_back((unicodeValue >> 12) | 0xE0);
            utfString.push_back(((unicodeValue >> 6) & 0x3F) | 0x80);
            utfString.push_back((unicodeValue & 0x3F) | 0x80);

            // Skip the next 5 characters ('\\', 'u', and the 4 hexadecimal digits)
            i += 5;
        } else {
            // Append the character as is
            utfString.push_back(asciiString[i]);
        };
    };
    return utfString;
}
