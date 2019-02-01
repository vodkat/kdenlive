/***************************************************************************
 *   Copyright (C) 2017 by Nicolas Carion                                  *
 *   This file is part of Kdenlive. See www.kdenlive.org.                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3 or any later version accepted by the       *
 *   membership of KDE e.V. (or its successor approved  by the membership  *
 *   of KDE e.V.), which shall act as a proxy defined in Section 14 of     *
 *   version 3 of the license.                                             *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#include "keyframemodel.hpp"
#include "rotoscoping/bpoint.h"
#include "rotoscoping/rotohelper.hpp"
#include "core.h"
#include "doc/docundostack.hpp"
#include "macros.hpp"

#include <QDebug>
#include <QJsonDocument>
#include <mlt++/Mlt.h>

KeyframeModel::KeyframeModel(std::weak_ptr<AssetParameterModel> model, const QModelIndex &index, std::weak_ptr<DocUndoStack> undo_stack, QObject *parent)
    : QAbstractListModel(parent)
    , m_model(std::move(model))
    , m_undoStack(std::move(undo_stack))
    , m_index(index)
    , m_lastData()
    , m_lock(QReadWriteLock::Recursive)
{
    qDebug() << "Construct keyframemodel. Checking model:" << m_model.expired();
    if (auto ptr = m_model.lock()) {
        m_paramType = ptr->data(m_index, AssetParameterModel::TypeRole).value<ParamType>();
    }
    setup();
    refresh();
}

void KeyframeModel::setup()
{
    // We connect the signals of the abstractitemmodel to a more generic one.
    connect(this, &KeyframeModel::columnsMoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::columnsRemoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::columnsInserted, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::rowsMoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::rowsRemoved, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::rowsInserted, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::modelReset, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::dataChanged, this, &KeyframeModel::modelChanged);
    connect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
}

bool KeyframeModel::addKeyframe(GenTime pos, KeyframeType type, QVariant value, bool notify, Fun &undo, Fun &redo)
{
    qDebug() << "ADD keyframe" << pos.frames(pCore->getCurrentFps()) << value << notify;
    QWriteLocker locker(&m_lock);
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    if (m_keyframeList.count(pos) > 0) {
        qDebug() << "already there";
        if (std::pair<KeyframeType, QVariant>({type, value}) == m_keyframeList.at(pos)) {
            qDebug() << "nothing to do";
            return true; // nothing to do
        }
        // In this case we simply change the type and value
        KeyframeType oldType = m_keyframeList[pos].first;
        QVariant oldValue = m_keyframeList[pos].second;
        local_undo = updateKeyframe_lambda(pos, oldType, oldValue, notify);
        local_redo = updateKeyframe_lambda(pos, type, value, notify);
    } else {
        qDebug() << "True addition";
        local_redo = addKeyframe_lambda(pos, type, value, notify);
        local_undo = deleteKeyframe_lambda(pos, notify);
    }
    if (local_redo()) {
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
        return true;
    }
    return false;
}

bool KeyframeModel::addKeyframe(int frame, double normalizedValue)
{
    if (auto ptr = m_model.lock()) {
        Q_ASSERT(m_index.isValid());
        double min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
        double max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
        double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
        double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
        int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
        double realValue;
        if (logRole == -1) {
            // Logarythmic scale for lower than norm values
            if (normalizedValue >= 0.5) {
                realValue = norm + (2 * (normalizedValue - 0.5) * (max / factor - norm));
            } else {
                realValue = norm - pow(2 * (0.5 - normalizedValue), 10.0 / 6) * (norm - min / factor);
            }
        } else {
            realValue = (normalizedValue * (max - min) + min) / factor;
        }
        // TODO: Use default configurable kf type
        return addKeyframe(GenTime(frame, pCore->getCurrentFps()), KeyframeType::Linear, realValue);
    }
    return false;
}

bool KeyframeModel::addKeyframe(GenTime pos, KeyframeType type, QVariant value)
{
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    bool update = (m_keyframeList.count(pos) > 0);
    bool res = addKeyframe(pos, type, value, true, undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, update ? i18n("Change keyframe type") : i18n("Add keyframe"));
    }
    return res;
}

bool KeyframeModel::removeKeyframe(GenTime pos, Fun &undo, Fun &redo, bool notify)
{
    qDebug() << "Going to remove keyframe at " << pos.frames(pCore->getCurrentFps());
    qDebug() << "before" << getAnimProperty();
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType oldType = m_keyframeList[pos].first;
    QVariant oldValue = m_keyframeList[pos].second;
    Fun local_undo = addKeyframe_lambda(pos, oldType, oldValue, notify);
    Fun local_redo = deleteKeyframe_lambda(pos, notify);
    qDebug() << "before2" << getAnimProperty();
    if (local_redo()) {
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
        return true;
    }
    return false;
}

bool KeyframeModel::removeKeyframe(int frame)
{
    GenTime pos(frame, pCore->getCurrentFps());
    return removeKeyframe(pos);
}

bool KeyframeModel::removeKeyframe(GenTime pos)
{
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    if (m_keyframeList.count(pos) > 0 && m_keyframeList.find(pos) == m_keyframeList.begin()) {
        return false; // initial point must stay
    }

    bool res = removeKeyframe(pos, undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, i18n("Delete keyframe"));
    }
    return res;
}

bool KeyframeModel::moveKeyframe(GenTime oldPos, GenTime pos, double newVal, Fun &undo, Fun &redo)
{
    qDebug() << "starting to move keyframe" << oldPos.frames(pCore->getCurrentFps()) << pos.frames(pCore->getCurrentFps());
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(oldPos) > 0);
    if (oldPos == pos) {
        double realValue;
        // Calculate real value from normalized
        if (auto ptr = m_model.lock()) {
            double min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
            double max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
            double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
            double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
            int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
            if (logRole == -1) {
                // Logarythmic scale for lower than norm values
                if (newVal >= 0.5) {
                    realValue = norm + (2 * (newVal - 0.5) * (max / factor - norm));
                } else {
                    realValue = norm - pow(2 * (0.5 - newVal), 10.0 / 6) * (norm - min / factor);
                }
            } else {
                realValue = (newVal * (max - min) + min) / factor;
            }
        }
        return updateKeyframe(pos, realValue);
    }
    KeyframeType oldType = m_keyframeList[oldPos].first;
    QVariant oldValue = m_keyframeList[oldPos].second;
    if (oldPos != pos && hasKeyframe(pos)) return false;
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    qDebug() << getAnimProperty();
    //TODO: use the new Animation::key_set_frame to move a keyframe
    bool res = removeKeyframe(oldPos, local_undo, local_redo);
    qDebug() << "Move keyframe finished deletion:" << res;
    qDebug() << getAnimProperty();
    if (res) {
        if (newVal > -1) {
            if (auto ptr = m_model.lock()) {
                double min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
                double max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
                double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
                double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
                int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
                double realValue;
                if (logRole == -1) {
                    // Logarythmic scale for lower than norm values
                    if (newVal >= 0.5) {
                        realValue = norm + (2 * (newVal - 0.5) * (max / factor - norm));
                    } else {
                        realValue = norm - pow(2 * (0.5 - newVal), 10.0 / 6) * (norm - min / factor);
                    }
                } else {
                    realValue = (newVal * (max - min) + min) / factor;
                }
                res = addKeyframe(pos, oldType, realValue, true, local_undo, local_redo);
            }
        } else {
            res = addKeyframe(pos, oldType, oldValue, true, local_undo, local_redo);
        }
        qDebug() << "Move keyframe finished insertion:" << res;
        qDebug() << getAnimProperty();
    }
    if (res) {
        UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
    } else {
        bool undone = local_undo();
        Q_ASSERT(undone);
    }
    return res;
}

bool KeyframeModel::moveKeyframe(int oldPos, int pos, bool logUndo)
{
    GenTime oPos(oldPos, pCore->getCurrentFps());
    GenTime nPos(pos, pCore->getCurrentFps());
    return moveKeyframe(oPos, nPos, -1, logUndo);
}

bool KeyframeModel::offsetKeyframes(int oldPos, int pos, bool logUndo)
{
    if (oldPos == pos) return true;
    GenTime oldFrame(oldPos, pCore->getCurrentFps());
    Q_ASSERT(m_keyframeList.count(oldFrame) > 0);
    GenTime diff(pos - oldPos, pCore->getCurrentFps());
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    QList<GenTime> times;
    for (const auto &m : m_keyframeList) {
        if (m.first < oldFrame) continue;
        times << m.first;
    }
    bool res = true;
    for (const auto &t : times) {
        res &= moveKeyframe(t, t + diff, -1, undo, redo);
    }
    if (res && logUndo) {
        PUSH_UNDO(undo, redo, i18n("Move keyframes"));
    }
    return res;
}

bool KeyframeModel::moveKeyframe(int oldPos, int pos, double newVal)
{
    GenTime oPos(oldPos, pCore->getCurrentFps());
    GenTime nPos(pos, pCore->getCurrentFps());
    return moveKeyframe(oPos, nPos, newVal, true);
}

bool KeyframeModel::moveKeyframe(GenTime oldPos, GenTime pos, double newVal, bool logUndo)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(oldPos) > 0);
    if (oldPos == pos) return true;
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool res = moveKeyframe(oldPos, pos, newVal, undo, redo);
    if (res && logUndo) {
        PUSH_UNDO(undo, redo, i18n("Move keyframe"));
    }
    return res;
}

bool KeyframeModel::directUpdateKeyframe(GenTime pos, QVariant value)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType type = m_keyframeList[pos].first;
    auto operation = updateKeyframe_lambda(pos, type, value, true);
    return operation();
}

bool KeyframeModel::updateKeyframe(GenTime pos, QVariant value, Fun &undo, Fun &redo, bool update)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType type = m_keyframeList[pos].first;
    QVariant oldValue = m_keyframeList[pos].second;
    // Check if keyframe is different
    if (m_paramType == ParamType::KeyframeParam) {
        if (qFuzzyCompare(oldValue.toDouble(), value.toDouble())) return true;
    }
    auto operation = updateKeyframe_lambda(pos, type, value, update);
    auto reverse = updateKeyframe_lambda(pos, type, oldValue, update);
    bool res = operation();
    if (res) {
        UPDATE_UNDO_REDO(operation, reverse, undo, redo);
    }
    return res;
}

bool KeyframeModel::updateKeyframe(int pos, double newVal)
{
    GenTime Pos(pos, pCore->getCurrentFps());
    if (auto ptr = m_model.lock()) {
        double min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
        double max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
        double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
        double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
        int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
        double realValue;
        if (logRole == -1) {
            // Logarythmic scale for lower than norm values
            if (newVal >= 0.5) {
                realValue = norm + (2 * (newVal - 0.5) * (max / factor - norm));
            } else {
                realValue = norm - pow(2 * (0.5 - newVal), 10.0 / 6) * (norm - min / factor);
            }
        } else {
            realValue = (newVal * (max - min) + min) / factor;
        }
        return updateKeyframe(Pos, realValue);
    }
    return false;
}

bool KeyframeModel::updateKeyframe(GenTime pos, QVariant value)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);

    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool res = updateKeyframe(pos, value, undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, i18n("Update keyframe"));
    }
    return res;
}

KeyframeType convertFromMltType(mlt_keyframe_type type)
{
    switch (type) {
    case mlt_keyframe_linear:
        return KeyframeType::Linear;
    case mlt_keyframe_discrete:
        return KeyframeType::Discrete;
    case mlt_keyframe_smooth:
        return KeyframeType::Curve;
    }
    return KeyframeType::Linear;
}

bool KeyframeModel::updateKeyframeType(GenTime pos, int type, Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    Q_ASSERT(m_keyframeList.count(pos) > 0);
    KeyframeType oldType = m_keyframeList[pos].first;
    KeyframeType newType = convertFromMltType((mlt_keyframe_type)type);
    QVariant value = m_keyframeList[pos].second;
    // Check if keyframe is different
    if (m_paramType == ParamType::KeyframeParam) {
        if (oldType == newType) return true;
    }
    auto operation = updateKeyframe_lambda(pos, newType, value, true);
    auto reverse = updateKeyframe_lambda(pos, oldType, value, true);
    bool res = operation();
    if (res) {
        UPDATE_UNDO_REDO(operation, reverse, undo, redo);
    }
    return res;
}

Fun KeyframeModel::updateKeyframe_lambda(GenTime pos, KeyframeType type, QVariant value, bool notify)
{
    QWriteLocker locker(&m_lock);
    return [this, pos, type, value, notify]() {
        qDebug() << "update lambda" << pos.frames(pCore->getCurrentFps()) << value << notify;
        Q_ASSERT(m_keyframeList.count(pos) > 0);
        int row = static_cast<int>(std::distance(m_keyframeList.begin(), m_keyframeList.find(pos)));
        m_keyframeList[pos].first = type;
        m_keyframeList[pos].second = value;
        if (notify) emit dataChanged(index(row), index(row), {ValueRole, NormalizedValueRole, TypeRole});
        return true;
    };
}

Fun KeyframeModel::addKeyframe_lambda(GenTime pos, KeyframeType type, QVariant value, bool notify)
{
    QWriteLocker locker(&m_lock);
    return [this, notify, pos, type, value]() {
        qDebug() << "add lambda" << pos.frames(pCore->getCurrentFps()) << value << notify;
        Q_ASSERT(m_keyframeList.count(pos) == 0);
        // We determine the row of the newly added marker
        auto insertionIt = m_keyframeList.lower_bound(pos);
        int insertionRow = static_cast<int>(m_keyframeList.size());
        if (insertionIt != m_keyframeList.end()) {
            insertionRow = static_cast<int>(std::distance(m_keyframeList.begin(), insertionIt));
        }
        if (notify) beginInsertRows(QModelIndex(), insertionRow, insertionRow);
        m_keyframeList[pos].first = type;
        m_keyframeList[pos].second = value;
        if (notify) endInsertRows();
        return true;
    };
}

Fun KeyframeModel::deleteKeyframe_lambda(GenTime pos, bool notify)
{
    QWriteLocker locker(&m_lock);
    return [this, pos, notify]() {
        qDebug() << "delete lambda" << pos.frames(pCore->getCurrentFps()) << notify;
        qDebug() << "before" << getAnimProperty();
        Q_ASSERT(m_keyframeList.count(pos) > 0);
        Q_ASSERT(pos != GenTime()); // cannot delete initial point
        int row = static_cast<int>(std::distance(m_keyframeList.begin(), m_keyframeList.find(pos)));
        if (notify) beginRemoveRows(QModelIndex(), row, row);
        m_keyframeList.erase(pos);
        if (notify) endRemoveRows();
        qDebug() << "after" << getAnimProperty();
        return true;
    };
}

QHash<int, QByteArray> KeyframeModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[PosRole] = "position";
    roles[FrameRole] = "frame";
    roles[TypeRole] = "type";
    roles[ValueRole] = "value";
    roles[NormalizedValueRole] = "normalizedValue";
    return roles;
}

QVariant KeyframeModel::data(const QModelIndex &index, int role) const
{
    READ_LOCK();
    if (index.row() < 0 || index.row() >= static_cast<int>(m_keyframeList.size()) || !index.isValid()) {
        return QVariant();
    }
    auto it = m_keyframeList.begin();
    std::advance(it, index.row());
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
    case ValueRole:
        return it->second.second;
    case NormalizedValueRole: {
        if (m_paramType == ParamType::AnimatedRect) {
            const QString &data = it->second.second.toString();
            QLocale locale;
            return locale.toDouble(data.section(QLatin1Char(' '), -1));
        }
        double val = it->second.second.toDouble();
        if (auto ptr = m_model.lock()) {
            Q_ASSERT(m_index.isValid());
            double min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
            double max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
            double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
            double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
            int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
            double linear = val * factor;
            if (logRole == -1) {
                // Logarythmic scale for lower than norm values
                if (linear >= norm) {
                    return 0.5 + (linear - norm) / (max * factor - norm) * 0.5;
                }
                // transform current value to 0..1 scale
                double scaled = (linear - norm) / (min * factor - norm);
                // Log scale
                return 0.5 - pow(scaled, 0.6) * 0.5;
            }
            return (linear - min) / (max - min);
        } else {
            qDebug() << "// CANNOT LOCK effect MODEL";
        }
        return 1;
    }
    case PosRole:
        return it->first.seconds();
    case FrameRole:
    case Qt::UserRole:
        return it->first.frames(pCore->getCurrentFps());
    case TypeRole:
        return QVariant::fromValue<KeyframeType>(it->second.first);
    }
    return QVariant();
}

int KeyframeModel::rowCount(const QModelIndex &parent) const
{
    READ_LOCK();
    if (parent.isValid()) return 0;
    return static_cast<int>(m_keyframeList.size());
}

bool KeyframeModel::singleKeyframe() const
{
    READ_LOCK();
    return m_keyframeList.size() <= 1;
}

Keyframe KeyframeModel::getKeyframe(const GenTime &pos, bool *ok) const
{
    READ_LOCK();
    if (m_keyframeList.count(pos) <= 0) {
        // return empty marker
        *ok = false;
        return {GenTime(), KeyframeType::Linear};
    }
    *ok = true;
    return {pos, m_keyframeList.at(pos).first};
}

Keyframe KeyframeModel::getNextKeyframe(const GenTime &pos, bool *ok) const
{
    auto it = m_keyframeList.upper_bound(pos);
    if (it == m_keyframeList.end()) {
        // return empty marker
        *ok = false;
        return {GenTime(), KeyframeType::Linear};
    }
    *ok = true;
    return {(*it).first, (*it).second.first};
}

Keyframe KeyframeModel::getPrevKeyframe(const GenTime &pos, bool *ok) const
{
    auto it = m_keyframeList.lower_bound(pos);
    if (it == m_keyframeList.begin()) {
        // return empty marker
        *ok = false;
        return {GenTime(), KeyframeType::Linear};
    }
    --it;
    *ok = true;
    return {(*it).first, (*it).second.first};
}

Keyframe KeyframeModel::getClosestKeyframe(const GenTime &pos, bool *ok) const
{
    if (m_keyframeList.count(pos) > 0) {
        return getKeyframe(pos, ok);
    }
    bool ok1, ok2;
    auto next = getNextKeyframe(pos, &ok1);
    auto prev = getPrevKeyframe(pos, &ok2);
    *ok = ok1 || ok2;
    if (ok1 && ok2) {
        double fps = pCore->getCurrentFps();
        if (qAbs(next.first.frames(fps) - pos.frames(fps)) < qAbs(prev.first.frames(fps) - pos.frames(fps))) {
            return next;
        }
        return prev;
    } else if (ok1) {
        return next;
    } else if (ok2) {
        return prev;
    }
    // return empty marker
    return {GenTime(), KeyframeType::Linear};
}

bool KeyframeModel::hasKeyframe(int frame) const
{
    return hasKeyframe(GenTime(frame, pCore->getCurrentFps()));
}
bool KeyframeModel::hasKeyframe(const GenTime &pos) const
{
    READ_LOCK();
    return m_keyframeList.count(pos) > 0;
}

bool KeyframeModel::removeAllKeyframes(Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    std::vector<GenTime> all_pos;
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    int kfrCount = m_keyframeList.size() - 1;
    // we trigger only one global remove/insertrow event
    Fun update_redo_start = [this, kfrCount]() {
        beginRemoveRows(QModelIndex(), 1, kfrCount);
        return true;
    };
    Fun update_redo_end = [this]() {
        endRemoveRows();
        return true;
    };
    Fun update_undo_start = [this, kfrCount]() {
        beginInsertRows(QModelIndex(), 1, kfrCount);
        return true;
    };
    Fun update_undo_end = [this]() {
        endInsertRows();
        return true;
    };
    PUSH_LAMBDA(update_redo_start, local_redo);
    PUSH_LAMBDA(update_undo_start, local_undo);
    for (const auto &m : m_keyframeList) {
        all_pos.push_back(m.first);
    }
    update_redo_start();
    bool res = true;
    bool first = true;
    for (const auto &p : all_pos) {
        if (first) { // skip first point
            first = false;
            continue;
        }
        res = removeKeyframe(p, local_undo, local_redo, false);
        if (!res) {
            bool undone = local_undo();
            Q_ASSERT(undone);
            return false;
        }
    }
    update_redo_end();
    PUSH_LAMBDA(update_redo_end, local_redo);
    PUSH_LAMBDA(update_undo_end, local_undo);
    UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
    return true;
}

bool KeyframeModel::removeAllKeyframes()
{
    QWriteLocker locker(&m_lock);
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    bool res = removeAllKeyframes(undo, redo);
    if (res) {
        PUSH_UNDO(undo, redo, i18n("Delete all keyframes"));
    }
    return res;
}

QString KeyframeModel::getAnimProperty() const
{
    if (m_paramType == ParamType::Roto_spline) {
        return getRotoProperty();
    }
    QString prop;
    bool first = true;
    QLocale locale;
    for (const auto keyframe : m_keyframeList) {
        if (first) {
            first = false;
        } else {
            prop += QStringLiteral(";");
        }
        prop += QString::number(keyframe.first.frames(pCore->getCurrentFps()));
        switch (keyframe.second.first) {
        case KeyframeType::Linear:
            prop += QStringLiteral("=");
            break;
        case KeyframeType::Discrete:
            prop += QStringLiteral("|=");
            break;
        case KeyframeType::Curve:
            prop += QStringLiteral("~=");
            break;
        }
        switch (m_paramType) {
        case ParamType::AnimatedRect:
            prop += keyframe.second.second.toString();
            break;
        default:
            prop += locale.toString(keyframe.second.second.toDouble());
            break;
        }
    }
    return prop;
}

QString KeyframeModel::getRotoProperty() const
{
    QJsonDocument doc;
    if (auto ptr = m_model.lock()) {
        int in = ptr->data(m_index, AssetParameterModel::ParentInRole).toInt();
        int out = ptr->data(m_index, AssetParameterModel::ParentDurationRole).toInt();
        QMap<QString, QVariant> map;
        for (const auto keyframe : m_keyframeList) {
            map.insert(QString::number(in + keyframe.first.frames(pCore->getCurrentFps())).rightJustified(log10((double)out) + 1, '0'), keyframe.second.second);
        }
        doc = QJsonDocument::fromVariant(QVariant(map));
    }
    return doc.toJson();
}

mlt_keyframe_type convertToMltType(KeyframeType type)
{
    switch (type) {
    case KeyframeType::Linear:
        return mlt_keyframe_linear;
    case KeyframeType::Discrete:
        return mlt_keyframe_discrete;
    case KeyframeType::Curve:
        return mlt_keyframe_smooth;
    }
    return mlt_keyframe_linear;
}

void KeyframeModel::parseAnimProperty(const QString &prop)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };
    Mlt::Properties mlt_prop;
    QLocale locale;
    disconnect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
    removeAllKeyframes(undo, redo);
    mlt_prop.set("key", prop.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop.anim_get_int("key", 0, 0);

    Mlt::Animation *anim = mlt_prop.get_anim("key");
    int in = 0;
    if (auto ptr = m_model.lock()) {
        in = ptr->data(m_index, AssetParameterModel::ParentInRole).toInt();
    }
    qDebug() << "Found" << anim->key_count() << "animation properties";
    for (int i = 0; i < anim->key_count(); ++i) {
        int frame;
        mlt_keyframe_type type;
        anim->key_get(i, frame, type);
        if (!prop.contains(QLatin1Char('='))) {
            // TODO: use a default user defined type
            type = mlt_keyframe_linear;
        }
        QVariant value;
        switch (m_paramType) {
        case ParamType::AnimatedRect: {
            mlt_rect rect = mlt_prop.anim_get_rect("key", frame);
            value = QVariant(QStringLiteral("%1 %2 %3 %4 %5").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h).arg(locale.toString(rect.o)));
            break;
        }
        default:
            value = QVariant(mlt_prop.anim_get_double("key", frame));
            break;
        }
        if (i == 0 && frame > in) {
            // Always add a keyframe at start pos
            addKeyframe(GenTime(in, pCore->getCurrentFps()), convertFromMltType(type), value, true, undo, redo);
        } else if (frame == in && hasKeyframe(GenTime(in))) {
            // First keyframe already exists, adjust its value
            updateKeyframe(GenTime(frame, pCore->getCurrentFps()), value, undo, redo, true);
            continue;
        }
        addKeyframe(GenTime(frame, pCore->getCurrentFps()), convertFromMltType(type), value, true, undo, redo);
    }
    delete anim;
    connect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
}


void KeyframeModel::resetAnimProperty(const QString &prop)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    // Delete all existing keyframes
    disconnect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
    removeAllKeyframes(undo, redo);

    Mlt::Properties mlt_prop;
    QLocale locale;
    int in = 0;
    if (auto ptr = m_model.lock()) {
        in = ptr->data(m_index, AssetParameterModel::ParentInRole).toInt();
    }
    mlt_prop.set("key", prop.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop.anim_get_int("key", 0, 0);

    Mlt::Animation *anim = mlt_prop.get_anim("key");

    qDebug() << "Found" << anim->key_count() << "animation properties";
    for (int i = 0; i < anim->key_count(); ++i) {
        int frame;
        mlt_keyframe_type type;
        anim->key_get(i, frame, type);
        if (!prop.contains(QLatin1Char('='))) {
            // TODO: use a default user defined type
            type = mlt_keyframe_linear;
        }
        QVariant value;
        switch (m_paramType) {
        case ParamType::AnimatedRect: {
            mlt_rect rect = mlt_prop.anim_get_rect("key", frame);
            value = QVariant(QStringLiteral("%1 %2 %3 %4 %5").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h).arg(locale.toString(rect.o)));
            break;
        }
        default:
            value = QVariant(mlt_prop.anim_get_double("key", frame));
            break;
        }
        if (i == 0 && frame > in) {
            // Always add a keyframe at start pos
            addKeyframe(GenTime(in, pCore->getCurrentFps()), convertFromMltType(type), value, false, undo, redo);
        } else if (frame == in && hasKeyframe(GenTime(in))) {
            // First keyframe already exists, adjust its value
            updateKeyframe(GenTime(frame, pCore->getCurrentFps()), value, undo, redo, false);
            continue;
        }
        addKeyframe(GenTime(frame, pCore->getCurrentFps()), convertFromMltType(type), value, false, undo, redo);
    }
    delete anim;
    QString effectName;
    if (auto ptr = m_model.lock()) {
        effectName = ptr->data(m_index, Qt::DisplayRole).toString();
    } else {
        effectName = i18n("effect");
    }
    Fun update_local = [this]() {
        emit dataChanged(index(0), index(m_keyframeList.size()), {});
        return true;
    };
    update_local();
    PUSH_LAMBDA(update_local, undo);
    PUSH_LAMBDA(update_local, redo);
    PUSH_UNDO(undo, redo, i18n("Reset %1", effectName));
    connect(this, &KeyframeModel::modelChanged, this, &KeyframeModel::sendModification);
}

void KeyframeModel::parseRotoProperty(const QString &prop)
{
    Fun undo = []() { return true; };
    Fun redo = []() { return true; };

    QJsonParseError jsonError;
    QJsonDocument doc = QJsonDocument::fromJson(prop.toLatin1(), &jsonError);
    QVariant data = doc.toVariant();
    if (data.canConvert(QVariant::Map)) {
        QList<int> keyframes;
        QMap<QString, QVariant> map = data.toMap();
        QMap<QString, QVariant>::const_iterator i = map.constBegin();
        while (i != map.constEnd()) {
            addKeyframe(GenTime(i.key().toInt(), pCore->getCurrentFps()), KeyframeType::Linear, i.value(), false, undo, redo);
            ++i;
        }
    }
}

QVariant KeyframeModel::getInterpolatedValue(int p) const
{
    auto pos = GenTime(p, pCore->getCurrentFps());
    return getInterpolatedValue(pos);
}

QVariant KeyframeModel::getNormalizedValue(double newVal) const
{
    if (auto ptr = m_model.lock()) {
        double min = ptr->data(m_index, AssetParameterModel::MinRole).toDouble();
        double max = ptr->data(m_index, AssetParameterModel::MaxRole).toDouble();
        double factor = ptr->data(m_index, AssetParameterModel::FactorRole).toDouble();
        double norm = ptr->data(m_index, AssetParameterModel::DefaultRole).toDouble();
        int logRole = ptr->data(m_index, AssetParameterModel::ScaleRole).toInt();
        double realValue;
        if (logRole == -1) {
            // Logarythmic scale for lower than norm values
            if (newVal >= 0.5) {
                realValue = norm + (2 * (newVal - 0.5) * (max / factor - norm));
            } else {
                realValue = norm - pow(2 * (0.5 - newVal), 10.0 / 6) * (norm - min / factor);
            }
        } else {
            realValue = (newVal * (max - min) + min) / factor;
        }
        return QVariant(realValue);
    }
    return QVariant();
}

QVariant KeyframeModel::getInterpolatedValue(const GenTime &pos) const
{
    if (m_keyframeList.count(pos) > 0) {
        return m_keyframeList.at(pos).second;
    }
    if (m_keyframeList.size() == 0) {
        return QVariant();
    }
    auto next = m_keyframeList.upper_bound(pos);
    if (next == m_keyframeList.cbegin()) {
        return (m_keyframeList.cbegin())->second.second;
    } else if (next == m_keyframeList.cend()) {
        auto it = m_keyframeList.cend();
        --it;
        return it->second.second;
    }
    auto prev = next;
    --prev;
    // We now have surrounding keyframes, we use mlt to compute the value
    Mlt::Properties prop;
    QLocale locale;
    int p = pos.frames(pCore->getCurrentFps());
    if (m_paramType == ParamType::KeyframeParam) {
        prop.anim_set("keyframe", prev->second.second.toDouble(), prev->first.frames(pCore->getCurrentFps()), next->first.frames(pCore->getCurrentFps()),
                      convertToMltType(prev->second.first));
        prop.anim_set("keyframe", next->second.second.toDouble(), next->first.frames(pCore->getCurrentFps()), next->first.frames(pCore->getCurrentFps()),
                      convertToMltType(next->second.first));
        return QVariant(prop.anim_get_double("keyframe", p));
    } else if (m_paramType == ParamType::AnimatedRect) {
        QStringList vals = prev->second.second.toString().split(QLatin1Char(' '));
        if (vals.count() >= 4) {
            mlt_rect rect;
            rect.x = vals.at(0).toInt();
            rect.y = vals.at(1).toInt();
            rect.w = vals.at(2).toInt();
            rect.h = vals.at(3).toInt();
            if (vals.count() > 4) {
                rect.o = locale.toDouble(vals.at(4));
            } else {
                rect.o = 1;
            }
            prop.anim_set("keyframe", rect, prev->first.frames(pCore->getCurrentFps()), next->first.frames(pCore->getCurrentFps()),
                      convertToMltType(prev->second.first));
        }
        vals = next->second.second.toString().split(QLatin1Char(' '));
        if (vals.count() >= 4) {
            mlt_rect rect;
            rect.x = vals.at(0).toInt();
            rect.y = vals.at(1).toInt();
            rect.w = vals.at(2).toInt();
            rect.h = vals.at(3).toInt();
            if (vals.count() > 4) {
                rect.o = locale.toDouble(vals.at(4));
            } else {
                rect.o = 1;
            }
            prop.anim_set("keyframe", rect, next->first.frames(pCore->getCurrentFps()), next->first.frames(pCore->getCurrentFps()),
                      convertToMltType(next->second.first));
        }
        mlt_rect rect = prop.anim_get_rect("keyframe", p);
        const QString res = QStringLiteral("%1 %2 %3 %4 %5").arg((int)rect.x).arg((int)rect.y).arg((int)rect.w).arg((int)rect.h).arg(locale.toString(rect.o));
        return QVariant(res);
    } else if (m_paramType == ParamType::Roto_spline) {
        // interpolate
        QSize frame = pCore->getCurrentFrameSize();
        QList<BPoint> p1 = RotoHelper::getPoints(prev->second.second, frame);
        qreal relPos = (p - prev->first.frames(pCore->getCurrentFps())) / (qreal)(((next->first - prev->first).frames(pCore->getCurrentFps())) + 1);
        QList<BPoint> p2 = RotoHelper::getPoints(next->second.second, frame);
        int count = qMin(p1.count(), p2.count());
        QList<QVariant> vlist;
        for (int i = 0; i < count; ++i) {
            BPoint bp;
            QList<QVariant> pl;
            for (int j = 0; j < 3; ++j) {
                if (p1.at(i)[j] != p2.at(i)[j]) {
                    bp[j] = QLineF(p1.at(i)[j], p2.at(i)[j]).pointAt(relPos);
                } else {
                    bp[j] = p1.at(i)[j];
                }
                pl << QVariant(QList<QVariant>() << QVariant(bp[j].x() / frame.width()) << QVariant(bp[j].y() / frame.height()));
            }
            vlist << QVariant(pl);
        }
        return vlist;
    }
    return QVariant();
}

void KeyframeModel::sendModification()
{
    if (auto ptr = m_model.lock()) {
        Q_ASSERT(m_index.isValid());
        QString name = ptr->data(m_index, AssetParameterModel::NameRole).toString();
        QString data;
        if (m_paramType == ParamType::KeyframeParam || m_paramType == ParamType::AnimatedRect || m_paramType == ParamType::Roto_spline) {
            data = getAnimProperty();
            ptr->setParameter(name, data, false);
        } else {
            Q_ASSERT(false); // Not implemented, TODO
        }
    }
}

void KeyframeModel::refresh()
{
    Q_ASSERT(m_index.isValid());
    QString animData;
    if (auto ptr = m_model.lock()) {
        animData = ptr->data(m_index, AssetParameterModel::ValueRole).toString();
    } else {
        qDebug() << "WARNING : unable to access keyframe's model";
        return;
    }
    if (animData == m_lastData) {
        // nothing to do
        qDebug()<<"// DATA WAS ALREADY PARSED, ABORTING REFRESH\n_________________";
        return;
    }
    if (m_paramType == ParamType::KeyframeParam || m_paramType == ParamType::AnimatedRect) {
        parseAnimProperty(animData);
    } else if (m_paramType == ParamType::Roto_spline) {
        parseRotoProperty(animData);
    } else {
        // first, try to convert to double
        bool ok = false;
        double value = animData.toDouble(&ok);
        if (ok) {
            Fun undo = []() { return true; };
            Fun redo = []() { return true; };
            addKeyframe(GenTime(), KeyframeType::Linear, QVariant(value), false, undo, redo);
        } else {
            Q_ASSERT(false); // Not implemented, TODO
        }
    }
    m_lastData = animData;
}

void KeyframeModel::reset()
{
    Q_ASSERT(m_index.isValid());
    QString animData;
    if (auto ptr = m_model.lock()) {
        animData = ptr->data(m_index, AssetParameterModel::ValueRole).toString();
    } else {
        qDebug() << "WARNING : unable to access keyframe's model";
        return;
    }
    if (animData == m_lastData) {
        // nothing to do
        qDebug()<<"// DATA WAS ALREADY PARSED, ABORTING\n_________________";
        return;
    }
    if (m_paramType == ParamType::KeyframeParam || m_paramType == ParamType::AnimatedRect) {
        qDebug() << "parsing keyframe" << animData;
        resetAnimProperty(animData);
    } else if (m_paramType == ParamType::Roto_spline) {
        //TODO: resetRotoProperty(animData);
    } else {
        // first, try to convert to double
        bool ok = false;
        double value = animData.toDouble(&ok);
        if (ok) {
            Fun undo = []() { return true; };
            Fun redo = []() { return true; };
            addKeyframe(GenTime(), KeyframeType::Linear, QVariant(value), false, undo, redo);
            PUSH_UNDO(undo, redo, i18n("Reset effect"));
            qDebug() << "KEYFRAME ADDED" << value;
        } else {
            Q_ASSERT(false); // Not implemented, TODO
        }
    }
    m_lastData = animData;
}

QList<QPoint> KeyframeModel::getRanges(const QString &animData)
{
    Mlt::Properties mlt_prop;
    QLocale locale;
    mlt_prop.set("key", animData.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop.anim_get_int("key", 0, 0);

    Mlt::Animation *anim = mlt_prop.get_anim("key");
    int frame;
    mlt_keyframe_type type;
    anim->key_get(0, frame, type);
    mlt_rect rect = mlt_prop.anim_get_rect("key", frame);
    QPoint pX(rect.x, rect.x);
    QPoint pY(rect.y, rect.y);
    QPoint pW(rect.w, rect.w);
    QPoint pH(rect.h, rect.h);
    QPoint pO(rect.o, rect.o);
    for (int i = 1; i < anim->key_count(); ++i) {
        anim->key_get(i, frame, type);
        if (!animData.contains(QLatin1Char('='))) {
            // TODO: use a default user defined type
            type = mlt_keyframe_linear;
        }
        rect = mlt_prop.anim_get_rect("key", frame);
        pX.setX(qMin((int)rect.x, pX.x()));
        pX.setY(qMax((int)rect.x, pX.y()));
        pY.setX(qMin((int)rect.y, pY.x()));
        pY.setY(qMax((int)rect.y, pY.y()));
        pW.setX(qMin((int)rect.w, pW.x()));
        pW.setY(qMax((int)rect.w, pW.y()));
        pH.setX(qMin((int)rect.h, pH.x()));
        pH.setY(qMax((int)rect.h, pH.y()));
        pO.setX(qMin((int)rect.o, pO.x()));
        pO.setY(qMax((int)rect.o, pO.y()));
        //value = QVariant(QStringLiteral("%1 %2 %3 %4 %5").arg(rect.x).arg(rect.y).arg(rect.w).arg(rect.h).arg(locale.toString(rect.o)));
    }
    QList<QPoint> result {pX, pY, pW, pH, pO};
    return result;
}


std::shared_ptr<Mlt::Properties> KeyframeModel::getAnimation(const QString &animData)
{
    std::shared_ptr<Mlt::Properties> mlt_prop(new Mlt::Properties());
    mlt_prop->set("key", animData.toUtf8().constData());
    // This is a fake query to force the animation to be parsed
    (void)mlt_prop->anim_get_rect("key", 0, 0);
    return mlt_prop;
}

bool KeyframeModel::removeNextKeyframes(GenTime pos, Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    std::vector<GenTime> all_pos;
    Fun local_undo = []() { return true; };
    Fun local_redo = []() { return true; };
    int firstPos = 0;
    for (const auto &m : m_keyframeList) {
        if (m.first <= pos) {
            firstPos++;
            continue;
        }
        all_pos.push_back(m.first);
    }
    int kfrCount = (int)all_pos.size();
    // we trigger only one global remove/insertrow event
    Fun update_redo_start = [this, firstPos, kfrCount]() {
        beginRemoveRows(QModelIndex(), firstPos, kfrCount);
        return true;
    };
    Fun update_redo_end = [this]() {
        endRemoveRows();
        return true;
    };
    Fun update_undo_start = [this, firstPos, kfrCount]() {
        beginInsertRows(QModelIndex(), firstPos, kfrCount);
        return true;
    };
    Fun update_undo_end = [this]() {
        endInsertRows();
        return true;
    };
    PUSH_LAMBDA(update_redo_start, local_redo);
    PUSH_LAMBDA(update_undo_start, local_undo);
    update_redo_start();
    bool res = true;
    for (const auto &p : all_pos) {
        res = removeKeyframe(p, local_undo, local_redo, false);
        if (!res) {
            bool undone = local_undo();
            Q_ASSERT(undone);
            return false;
        }
    }
    update_redo_end();
    PUSH_LAMBDA(update_redo_end, local_redo);
    PUSH_LAMBDA(update_undo_end, local_undo);
    UPDATE_UNDO_REDO(local_redo, local_undo, undo, redo);
    return true;
}
