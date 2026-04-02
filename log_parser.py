"""
FastReact – Serial Log Parser & Test Analyser
=============================================
Reads timestamped serial logs from the server and player nodes,
then computes per-test metrics for the poster table.

Usage
-----
  python log_parser.py --server server.log --players p0.log p1.log [p2.log ...]

Log format expected (produced by the LOG() macro in game_protocol.h):
  [  1234] ACCEPTED PRESS from player 0 | reaction_ms=312 hop=1
  [  1250] SEND to AA:BB:CC:DD:EE:FF: OK
  ...

The script extracts:
  - Packet Success Rate  : (ACK-confirmed sends) / (total send attempts) per role
  - Round-trip latency   : time from GO send (server log) to PRESS received (server log)
  - Reaction fairness    : difference between actual fastest press and server-recorded times
  - Hop count logged in PRESS packets
  - Throughput estimate  : packets / second during active round window
"""

import re
import sys
import argparse
import statistics
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

# ── Regex patterns matching the LOG() macro output ──────────────────────────
RE_TIMESTAMP   = re.compile(r'^\[\s*(\d+)\]')
RE_SEND_OK     = re.compile(r'SEND to .+: OK')
RE_SEND_FAIL   = re.compile(r'SEND to .+: FAIL')
RE_GO_SENT     = re.compile(r'GO: sent to player (\d+)')
RE_PRESS_RECV  = re.compile(r'ACCEPTED PRESS from player (\d+) \| reaction_ms=(\d+) hop=(\d+)')
RE_ACK_SENT    = re.compile(r'PRESS ACK')
RE_ROUND_START = re.compile(r'Round is live\. GO sent to (\d+)/(\d+)')
RE_ROUND_END   = re.compile(r'=== ROUND RESULT ===')
RE_WINNER      = re.compile(r'>> WINNER: Player (\d+)')
RE_TIE         = re.compile(r'>> TIE')
RE_NO_WINNER   = re.compile(r'>> NO WINNER')
RE_RESULT_ROW  = re.compile(r'Player (\d+) .+ reactionMs=(\d+) .+ diff=\+(\d+) ms')
RE_TIMEOUT     = re.compile(r'ROUND TIMEOUT after (\d+) ms')
RE_DROP        = re.compile(r'DROP:')
RE_ACK_TIMEOUT = re.compile(r'ACK timeout')
RE_RREQ_SENT   = re.compile(r'RREQ \(id=\d+ ttl=\d+\)')
RE_RREP_DONE   = re.compile(r'route discovery complete')
RE_REPLAY_DROP = re.compile(r'DROP: duplicate')
RE_AUTH_FAIL   = re.compile(r'DROP: authentication failure')
RE_VERSION_FAIL= re.compile(r'DROP: incompatible protocol version')

@dataclass
class Round:
    round_num: int
    start_ts: Optional[int] = None        # millis when GO sent
    end_ts:   Optional[int] = None        # millis when ROUND RESULT logged
    players_sent_go: int = 0
    players_registered: int = 0
    presses: dict = field(default_factory=dict)  # player_idx -> {reaction_ms, hop, ts}
    winner: Optional[int] = None
    is_tie: bool = False
    timed_out: bool = False
    timeout_ms: Optional[int] = None

@dataclass
class Stats:
    label: str
    send_ok: int = 0
    send_fail: int = 0
    drops: int = 0
    ack_timeouts: int = 0
    replay_drops: int = 0
    auth_failures: int = 0
    version_failures: int = 0
    rounds: list = field(default_factory=list)  # list[Round]

def ts(line: str) -> Optional[int]:
    m = RE_TIMESTAMP.match(line)
    return int(m.group(1)) if m else None

def parse_log(path: str, label: str) -> Stats:
    stats = Stats(label=label)
    current_round: Optional[Round] = None
    round_count = 0

    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip()
            t = ts(line)

            # Send outcomes
            if RE_SEND_OK.search(line):
                stats.send_ok += 1
            if RE_SEND_FAIL.search(line):
                stats.send_fail += 1

            # Drop events
            if RE_DROP.search(line):
                stats.drops += 1
            if RE_REPLAY_DROP.search(line):
                stats.replay_drops += 1
            if RE_AUTH_FAIL.search(line):
                stats.auth_failures += 1
            if RE_VERSION_FAIL.search(line):
                stats.version_failures += 1

            # ACK timeouts (player side)
            if RE_ACK_TIMEOUT.search(line):
                stats.ack_timeouts += 1

            # Round lifecycle (server log)
            m = RE_ROUND_START.search(line)
            if m:
                round_count += 1
                current_round = Round(round_num=round_count,
                                      start_ts=t,
                                      players_sent_go=int(m.group(1)),
                                      players_registered=int(m.group(2)))
                stats.rounds.append(current_round)

            if current_round:
                m = RE_PRESS_RECV.search(line)
                if m:
                    pidx = int(m.group(1))
                    current_round.presses[pidx] = {
                        'reaction_ms': int(m.group(2)),
                        'hop': int(m.group(3)),
                        'ts': t,
                    }

                m = RE_WINNER.search(line)
                if m:
                    current_round.winner = int(m.group(1))

                if RE_TIE.search(line):
                    current_round.is_tie = True

                if RE_NO_WINNER.search(line):
                    current_round.winner = None

                m = RE_TIMEOUT.search(line)
                if m:
                    current_round.timed_out = True
                    current_round.timeout_ms = int(m.group(1))

                if RE_ROUND_END.search(line):
                    current_round.end_ts = t

    return stats

def packet_success_rate(s: Stats) -> str:
    total = s.send_ok + s.send_fail
    if total == 0:
        return "N/A (no sends recorded)"
    pct = 100.0 * s.send_ok / total
    return f"{pct:.1f}%  ({s.send_ok}/{total} OK)"

def round_latency_summary(rounds: list) -> dict:
    """
    For each round where we have both a start_ts and at least one press ts,
    compute the round-trip latency: time from GO send to first PRESS received.
    """
    rtts = []
    for r in rounds:
        if r.start_ts is None or not r.presses:
            continue
        first_press_ts = min(p['ts'] for p in r.presses.values() if p['ts'] is not None)
        rtts.append(first_press_ts - r.start_ts)

    if not rtts:
        return {}
    return {
        'count': len(rtts),
        'min_ms': min(rtts),
        'max_ms': max(rtts),
        'mean_ms': round(statistics.mean(rtts), 1),
        'stdev_ms': round(statistics.stdev(rtts), 1) if len(rtts) > 1 else 0,
    }

def fairness_analysis(rounds: list) -> list:
    """
    For each round, compare player reaction_ms values.
    Returns a list of dicts: round_num, fastest_ms, slowest_ms, spread_ms, hop_counts.
    """
    results = []
    for r in rounds:
        if not r.presses:
            continue
        times = {k: v['reaction_ms'] for k, v in r.presses.items()}
        hops  = {k: v['hop']         for k, v in r.presses.items()}
        fastest = min(times.values())
        slowest = max(times.values())
        results.append({
            'round': r.round_num,
            'fastest_ms': fastest,
            'slowest_ms': slowest,
            'spread_ms': slowest - fastest,
            'players': times,
            'hops': hops,
            'winner': r.winner,
            'is_tie': r.is_tie,
            'timed_out': r.timed_out,
        })
    return results

def throughput_estimate(rounds: list) -> str:
    """
    Rough packets/second within each round window (start_ts to end_ts).
    Counts: 1 GO per player + 1 PRESS per player + 1 ACK per player + 1 RESULT per player = ~4 per player.
    """
    estimates = []
    for r in rounds:
        if r.start_ts is None or r.end_ts is None or r.end_ts <= r.start_ts:
            continue
        duration_s = (r.end_ts - r.start_ts) / 1000.0
        n_players = r.players_sent_go
        # GO + PRESS + ACK + RESULT = 4 packets per player minimum
        estimated_pkts = n_players * 4
        estimates.append(estimated_pkts / duration_s)
    if not estimates:
        return "N/A"
    return f"avg {statistics.mean(estimates):.1f} pkt/s  (min {min(estimates):.1f}, max {max(estimates):.1f})"

def print_separator(char='─', width=72):
    print(char * width)

def print_report(server_stats: Stats, player_stats_list: list):
    width = 72
    print_separator('═', width)
    print("  FastReact – Test Results Report")
    print_separator('═', width)

    # ── PACKET SUCCESS RATE ────────────────────────────────────────────────
    print("\n📦 PACKET SUCCESS RATE")
    print_separator()
    print(f"  [SERVER]  {server_stats.label}")
    print(f"    Send success rate : {packet_success_rate(server_stats)}")
    print(f"    Auth failures     : {server_stats.auth_failures}")
    print(f"    Version mismatches: {server_stats.version_failures}")
    print(f"    Replay drops      : {server_stats.replay_drops}")
    print(f"    Total drops       : {server_stats.drops}")

    for ps in player_stats_list:
        print(f"\n  [PLAYER]  {ps.label}")
        print(f"    Send success rate : {packet_success_rate(ps)}")
        print(f"    ACK timeouts      : {ps.ack_timeouts}  (triggered retry / re-route)")
        print(f"    Auth failures     : {ps.auth_failures}")
        print(f"    Replay drops      : {ps.replay_drops}")

    # ── ROUND LATENCY ─────────────────────────────────────────────────────
    print("\n\n⏱  ROUND-TRIP LATENCY  (server: GO sent → first PRESS received)")
    print_separator()
    lat = round_latency_summary(server_stats.rounds)
    if lat:
        print(f"  Rounds analysed : {lat['count']}")
        print(f"  Min             : {lat['min_ms']} ms")
        print(f"  Max             : {lat['max_ms']} ms")
        print(f"  Mean            : {lat['mean_ms']} ms")
        print(f"  Std-dev         : {lat['stdev_ms']} ms")
    else:
        print("  No complete round data found in server log.")

    # ── FAIRNESS ──────────────────────────────────────────────────────────
    print("\n\n⚖  FAIRNESS ANALYSIS  (reaction_ms spread per round)")
    print_separator()
    fa = fairness_analysis(server_stats.rounds)
    if fa:
        for entry in fa:
            status = "TIMEOUT" if entry['timed_out'] else ("TIE" if entry['is_tie'] else f"P{entry['winner']} wins")
            print(f"  Round {entry['round']:>2}  [{status}]")
            print(f"    Fastest : {entry['fastest_ms']} ms   Slowest : {entry['slowest_ms']} ms   Spread : {entry['spread_ms']} ms")
            for pidx, rms in entry['players'].items():
                hop = entry['hops'].get(pidx, '?')
                print(f"      P{pidx}: {rms:>5} ms  (hop={hop})")
        spreads = [e['spread_ms'] for e in fa]
        print(f"\n  Summary — spread across {len(spreads)} round(s):")
        print(f"    Mean spread : {statistics.mean(spreads):.1f} ms")
        if len(spreads) > 1:
            print(f"    Max spread  : {max(spreads)} ms")
    else:
        print("  No round press data found in server log.")

    # ── DATA THROUGHPUT ───────────────────────────────────────────────────
    print("\n\n📡 DATA THROUGHPUT ESTIMATE")
    print_separator()
    print(f"  {throughput_estimate(server_stats.rounds)}")
    print("  (Estimated from: 4 packets × players per round ÷ round duration)")
    print("  ESP-NOW max theoretical: ~1 Mbps raw; effective ~200 kbps with overhead")
    print("  Each GamePacket = 32 bytes; at 10 pkt/s → ~2.56 kbps application layer")

    # ── POSTER TABLE SUMMARY ──────────────────────────────────────────────
    print("\n\n📋 POSTER TABLE SUMMARY")
    print_separator('═', width)
    print(f"  {'Metric':<35} {'No Relay':>16}  {'With Relay':>16}")
    print_separator()

    srv_psr = packet_success_rate(server_stats)
    # Attempt to split relay vs non-relay based on hop count in rounds
    no_relay_rounds  = [r for r in server_stats.rounds
                        if r.presses and all(v['hop'] <= 1 for v in r.presses.values())]
    relay_rounds     = [r for r in server_stats.rounds
                        if r.presses and any(v['hop'] > 1  for v in r.presses.values())]

    def psr_for(rounds_subset):
        # Re-compute from rounds that had no drop (all active players pressed)
        ok = sum(1 for r in rounds_subset
                 if r.presses and len(r.presses) == r.players_sent_go)
        total = len(rounds_subset)
        if total == 0:
            return "N/A"
        return f"{100*ok/total:.0f}% ({ok}/{total} rounds complete)"

    def lat_for(rounds_subset):
        lat = round_latency_summary(rounds_subset)
        if not lat:
            return "N/A"
        return f"{lat['mean_ms']} ms avg (n={lat['count']})"

    print(f"  {'Packet Success Rate':<35} {psr_for(no_relay_rounds):>16}  {psr_for(relay_rounds):>16}")
    print(f"  {'Round-trip Latency (GO→PRESS)':<35} {lat_for(no_relay_rounds):>16}  {lat_for(relay_rounds):>16}")
    print(f"  {'ACK Timeouts (player-side)':<35} {server_stats.ack_timeouts:>16}  {'(see player logs)':>16}")
    print(f"  {'Auth / Replay Drops':<35} {server_stats.auth_failures + server_stats.replay_drops:>16}  {'':>16}")
    print_separator('═', width)
    print()

def main():
    parser = argparse.ArgumentParser(
        description="FastReact serial log parser — computes poster test metrics")
    parser.add_argument('--server',  required=True, help='Server serial log file')
    parser.add_argument('--players', nargs='+', required=True,
                        help='One or more player serial log files')
    args = parser.parse_args()

    server_stats = parse_log(args.server, Path(args.server).stem)
    player_stats = [parse_log(p, Path(p).stem) for p in args.players]

    print_report(server_stats, player_stats)

if __name__ == '__main__':
    main()
