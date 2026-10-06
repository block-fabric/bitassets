// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CPUMINER_H
#define BITCOIN_NODE_CPUMINER_H

#include <script/script.h>
#include <sync.h>
#include <util/time.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace node {
struct NodeContext;

/**
 * Built-in miner, which searches for blocks with threads of the node's own
 * CPU. It is meant for test networks and for bootstrapping a new network; it
 * is no match for dedicated hardware.
 */
class CpuMiner
{
public:
    struct Stats {
        bool running{false};
        int threads{0};
        //! Hashes per second, measured since the previous call of GetStats() that took a measurement.
        double hashes_per_second{0};
        uint64_t hashes{0};
        uint64_t blocks_found{0};
        //! Blocks that were found but rejected by validation.
        uint64_t blocks_rejected{0};
        CScript coinbase_output_script;
    };

    explicit CpuMiner(NodeContext& node) : m_node{node} {}
    ~CpuMiner() { Stop(); }

    /**
     * Start mining with `threads` threads, replacing any mining in progress.
     * @return false, with `error` set, if mining is not possible.
     */
    bool Start(int threads, const CScript& coinbase_output_script, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    Stats GetStats() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    void Run(int thread, int threads, CScript coinbase_output_script);

    NodeContext& m_node;
    Mutex m_mutex;
    std::vector<std::thread> m_threads GUARDED_BY(m_mutex);
    CScript m_coinbase_output_script GUARDED_BY(m_mutex);
    std::atomic<bool> m_stop{false};
    std::atomic<uint64_t> m_hashes{0};
    std::atomic<uint64_t> m_blocks_found{0};
    std::atomic<uint64_t> m_blocks_rejected{0};
    //! The previous hash rate measurement.
    SteadyClock::time_point m_sample_time GUARDED_BY(m_mutex);
    uint64_t m_sample_hashes GUARDED_BY(m_mutex){0};
    double m_hashes_per_second GUARDED_BY(m_mutex){0};
};

} // namespace node

#endif // BITCOIN_NODE_CPUMINER_H
