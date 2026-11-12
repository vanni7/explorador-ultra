// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/DirectoryLister.h"

#include <QElapsedTimer>

namespace eu::fs {

/// Acumula elementos y los entrega por lotes según ListOptions.
/// Lo comparten las implementaciones de cada plataforma.
class BatchSender
{
public:
    BatchSender(const ListOptions &options, const BatchCallback &onBatch)
        : m_options(options), m_onBatch(onBatch)
    {
        m_batch.reserve(size_t(options.firstBatchSize));
        m_sinceLastBatch.start();
    }

    void add(Entry &&entry)
    {
        m_batch.push_back(std::move(entry));
        const int limit = m_firstBatchSent ? m_options.batchSize : m_options.firstBatchSize;
        if (int(m_batch.size()) >= limit || m_sinceLastBatch.elapsed() >= m_options.maxBatchDelayMs) {
            flush();
        }
    }

    void flush()
    {
        if (m_batch.empty()) {
            return;
        }
        std::vector<Entry> ready;
        ready.swap(m_batch);
        m_batch.reserve(size_t(m_options.batchSize));
        m_firstBatchSent = true;
        m_sinceLastBatch.restart();
        m_onBatch(std::move(ready));
    }

private:
    const ListOptions &m_options;
    const BatchCallback &m_onBatch;
    std::vector<Entry> m_batch;
    QElapsedTimer m_sinceLastBatch;
    bool m_firstBatchSent = false;
};

} // namespace eu::fs
