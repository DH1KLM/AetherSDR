#pragma once

#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <memory>
#include <utility>

namespace AetherSDR::anan {
// The owner drains consumers and then this worker before deleting the context.
// Queued transfers may release their last reference on any consumer thread.
template<class T> std::shared_ptr<T> retainPlannedResource(std::unique_ptr<T> value, QObject* planner)
{
    if (!planner) { return std::shared_ptr<T>(std::move(value)); }
    return std::shared_ptr<T>(value.release(), [planner](T* pointer) {
        auto release = [owned = std::unique_ptr<T>(pointer)]() mutable { owned.reset(); };
        if (QThread::currentThread() == planner->thread()) { release(); }
        else { QMetaObject::invokeMethod(planner, std::move(release), Qt::QueuedConnection); }
    });
}
} // namespace AetherSDR::anan
