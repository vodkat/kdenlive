/*
Copyright (C) 2012  Till Theato <root@ttill.de>
This file is part of kdenlive. See www.kdenlive.org.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef ABSTRACTEFFECTREPOSITORYITEM_H
#define ABSTRACTEFFECTREPOSITORYITEM_H

#include "effectrepository.h"
#include <QString>

namespace Mlt
{
    class Repository;
}

/**
 * tbd when groups are implemented
 */

class AbstractEffectRepositoryItem
{
public:
    AbstractEffectRepositoryItem();
    virtual ~AbstractEffectRepositoryItem();

    EffectTypes getType() const;
    static EffectTypes getType(const QString &type);
    QString getId() const;
    bool isValid() const;

protected:
    bool m_valid;
    QString m_id;
    EffectTypes m_type;
};

#endif
