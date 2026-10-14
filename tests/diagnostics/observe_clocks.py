#!/usr/bin/env python3
"""Run bounded serial diagnostics while sampling realtime against monotonic time."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('cmake-build-debug'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rounds', type=int, choices=range(1, 11), default=3)
    parser.add_argument("--serve-only", action="store_true", help="Observe only ServeProcess with shutdown tracing")
    args = parser.parse_args()
    build = args.build.resolve()
    commands = [(name, [str(build / 'anof/transport/http/tests' / name), '--log_level=test_suite'])
                for name in ('anof_http_identity_handlers_unittest',
                             'anof_http_identity_stream_unittest',
                             'anof_http_account_writes_unittest',
                             'anof_http_create_directory_unittest')]
    commands.append(('anof_serve_process_integration_unittest',
                     [str(build / 'tests/integration/anof_serve_process_integration_unittest'),
                      '--log_level=test_suite']))
    commands.append(('anof_timer_probe', [str(build / 'tests/diagnostics/anof_timer_probe')]))
    if args.serve_only:
        commands = [item for item in commands if item[0] == "anof_serve_process_integration_unittest"]
    for _, command in commands:
        if not Path(command[0]).is_file():
            parser.error(f'Missing executable: {command[0]}')
    args.output.mkdir(parents=True, exist_ok=False)
    stopped = threading.Event()
    lock = threading.Lock()
    with (args.output / 'timeline.jsonl').open('w') as timeline:
        def record(event, **fields):
            with lock:
                mono = time.monotonic_ns()
                wall = time.time_ns()
                timeline.write(json.dumps(dict(event=event, mono_ns=mono, wall_ns=wall, **fields)) + '\n')
                timeline.flush()

        def sample():
            previous = None
            while not stopped.is_set():
                mono = time.monotonic_ns()
                wall = time.time_ns()
                offset = wall - mono
                record('sample', sample_mono_ns=mono, sample_wall_ns=wall,
                       offset_ns=offset, offset_change_ns=0 if previous is None else offset-previous)
                previous = offset
                stopped.wait(0.02)

        sampler = threading.Thread(target=sample)
        sampler.start()
        try:
            for turn in range(1, args.rounds + 1):
                for name, command in commands:
                    label = f'{turn:02d}-{name}'
                    record('start', label=label, command=command)
                    with (args.output / f'{label}.log').open('w') as output:
                        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                                   env=dict(os.environ, ANOF_WAIT_TRACE='1', ANOF_SHUTDOWN_TRACE='1'),
                                                   start_new_session=True)
                        timeout = False
                        try:
                            process.wait(timeout=45 if name != 'anof_timer_probe' else 15)
                        except subprocess.TimeoutExpired:
                            timeout = True
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
                        finally:
                            if process.poll() is None:
                                os.killpg(process.pid, signal.SIGKILL)
                                process.wait()
                    record('end', label=label, returncode=process.returncode, timeout=timeout)
                    print(f'{label}: code={process.returncode} timeout={timeout}', flush=True)
        finally:
            stopped.set()
            sampler.join()


if __name__ == '__main__':
    main()
