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

#include "itemcategories.h"

#include <QByteArray>
#include <QString>

#include <map>

#include <QsLog/QsLog.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include "filters.h"
#include "util/util.h"

class CATEGORY_DATA {
private:
    CATEGORY_DATA() = default;
public:
    static CATEGORY_DATA& instance() {
        QLOG_TRACE() << "CATEGORY_DATA::instance() entered";
        static CATEGORY_DATA data;
        return data;
    };
    std::map<QString, QString> m_itemClassKeyToValue;
    std::map<QString, QString> m_itemClassValueToKey;
    std::map<QString, QString> m_itemBaseTypeToClass;
    QStringList categories;
};

void InitItemClasses(const QByteArray& classes) {

    static bool classes_initialized = false;

    QLOG_DEBUG() << "Initializing item classes";
    rapidjson::Document doc;
    doc.Parse(classes.constData());
    if (doc.HasParseError()) {
        const auto error = doc.GetParseError();
        const auto reason = rapidjson::GetParseError_En(error);
        QLOG_ERROR() << "Error parsing RePoE item classes:" << reason;
        return;
    };

    QLOG_INFO() << "Loading item classes from RePoE.";
    if (classes_initialized) {
        QLOG_WARN() << "Item classes have already been loaded. They will be overwritten.";
    };

    auto& data = CATEGORY_DATA::instance();
    data.m_itemClassKeyToValue.clear();
    data.m_itemClassValueToKey.clear();

    QLOG_TRACE() << "InitItemClasses() processing data";
    QSet<QString> cats;
    for (auto itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr) {
        const QString key = itr->name.GetString();
        const QString value = itr->value.FindMember("name")->value.GetString();
        if (key.startsWith("DONOTUSE") || (0 == key.compare("Unarmed", Qt::CaseInsensitive))) {
            continue;
        };
        if (value.isEmpty()) {
            QLOG_DEBUG() << "Item class for" << key << "is empty";
            continue;
        };
        data.m_itemClassKeyToValue[key] = value;
        data.m_itemClassValueToKey[value] = key;
        cats.insert(value);
    };
    data.categories = cats.values();
    data.categories.append(CategorySearchFilter::k_Default);
    data.categories.sort();

    classes_initialized = true;
}

void InitItemBaseTypes(const QByteArray& baseTypes) {

    static bool basetypes_initialized = false;
    
    QLOG_DEBUG() << "Initializing item base types";
    rapidjson::Document doc;
    doc.Parse(baseTypes.constData());
    if (doc.HasParseError()) {
        const auto error = doc.GetParseError();
        const auto reason = rapidjson::GetParseError_En(error);
        QLOG_ERROR() << "Error parsing RePoE item base types:" << reason;
        return;
    };

    QLOG_INFO() << "Loading item base types from RePoE.";
    if (basetypes_initialized) {
        QLOG_WARN() << "Item base types have already been loaded. They will be overwritten.";
    };

    QLOG_TRACE() << "InitItemBaseTypes() processing data";
    auto& data = CATEGORY_DATA::instance();
    data.m_itemBaseTypeToClass.clear();
    for (auto itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr) {
        // Skip unreleased objects.
        const QString release_state = itr->value.FindMember("release_state")->value.GetString();
        if (0 == release_state.compare("unreleased", Qt::CaseInsensitive)) {
            continue;
        };
        const QString item_class = itr->value.FindMember("item_class")->value.GetString();
        const QString name = itr->value.FindMember("name")->value.GetString();
        if (name.isEmpty() || name.startsWith("[DO NOT USE]") || name.startsWith("[UNUSED]") || name.startsWith("[DNT]")) {
            continue;
        };
        data.m_itemBaseTypeToClass[name] = item_class;
    };

    basetypes_initialized = true;
}

QString GetItemCategory(const QString& baseType) {

    auto& data = CATEGORY_DATA::instance();

    if (data.m_itemClassKeyToValue.empty()) {
        QLOG_ERROR() << "Item classes have not been initialized";
        return "";
    };

    if (data.m_itemBaseTypeToClass.empty()) {
        QLOG_ERROR() << "Item base types have not been initialized";
        return "";
    };

    auto rslt = data.m_itemBaseTypeToClass.find(baseType);
    if (rslt != data.m_itemBaseTypeToClass.end()) {
        QString key = rslt->second;
        rslt = data.m_itemClassKeyToValue.find(key);
        if (rslt != data.m_itemClassKeyToValue.end()) {
            QString category = rslt->second.toLower();
            QLOG_TRACE() << "GetItemCategory: category is" << category;
            return category;
        };
    };

    QLOG_TRACE() << "GetItemCategory: could not categorize baseType:" << baseType;
    return "";
}

const QStringList& GetItemCategories() {
    QLOG_TRACE() << "GetItemCategories() entered";
    auto& data = CATEGORY_DATA::instance();
    if (data.categories.isEmpty()) {
        QLOG_ERROR() << "Item categories have not been initialized";
    };
    return data.categories;
}
