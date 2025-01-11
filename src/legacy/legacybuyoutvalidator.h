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

#pragma once

#include <QString>

#include <map>
#include <set>

#include "legacydatastore.h"

class LegacyBuyoutValidator {
public:

    enum struct ValidationResult { Valid, Invalid, Error };

    LegacyBuyoutValidator(const QString& filename);
    ValidationResult status() { return m_status; };

private:
    void validateTabBuyouts();
    void validateItemBuyouts();

    const QString m_filename;
    const LegacyDataStore m_datastore;
    ValidationResult m_status;

    std::map<QString, std::set<QString>> m_issues;
};
