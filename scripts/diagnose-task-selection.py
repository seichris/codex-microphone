#!/usr/bin/env python3
"""Show local task identities without confusing run state with selection.

Reads the companion's private diagnostic log and authenticated loopback bridge.
Does not change selection, restart the companion, or log task contents/tokens.
"""
import argparse
import datetime
import json
from pathlib import Path
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        return None


def report(snapshot, diagnostic):
    tasks = {task['id']: task for task in snapshot['tasks']}
    def identify(thread_id):
        return tasks.get(thread_id, {'id': thread_id, 'title': None, 'status': 'unknown'})
    candidates = [{**identify(item['threadId']), 'host': item['hostId'],
                   'clientId': item['clientId']} if item['hostId'] == 'local' else
                  {'id': item['threadId'], 'title': None, 'status': 'unknown',
                   'host': item['hostId'], 'clientId': item['clientId']}
                  for item in diagnostic.get('candidateTasks', [])]
    return {
        'generatedAt': snapshot['generatedAt'],
        'connected': snapshot['connected'],
        'sourceError': snapshot.get('sourceError'),
        'runStateSource': snapshot.get('runStateSource', 'bridge-app-server'),
        'runStateNote': 'No observed running tasks does not prove that no tasks are running in other Codex clients.',
        'selectedTarget': snapshot.get('selection'),
        'selectionNote': 'A device-selected target or stream-derived candidate is not proof of keyboard focus.',
        'observerReason': diagnostic.get('reason'),
        'candidateCount': diagnostic.get('candidates'),
        'candidateTasks': candidates,
        'runningTasks': [task for task in tasks.values() if task['status'] == 'running'],
        'waitingTasks': [task for task in tasks.values() if task['status'] in ('waiting_input', 'waiting_approval')],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path.home() / 'Library/Application Support/Codex ESP32 Display/bridge-config.json')
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    port = int(config.get('port', 5180))
    if not 1 <= port <= 65535:
        raise ValueError('Invalid local bridge port')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    request = urllib.request.Request(f'http://127.0.0.1:{port}/api/v1/admin/task-diagnostics',
                                     headers={'Authorization': 'Bearer ' + config['token']})
    with opener.open(request, timeout=5) as response:
        snapshot = json.load(response)
    path = Path.home() / 'Library/Logs/CodexESP32Display/focused-task.log'
    # Read only the bounded end of the rotating log.
    with path.open('rb') as stream:
        stream.seek(0, 2)
        stream.seek(max(0, stream.tell() - 65536))
        lines = stream.read().decode('utf-8', errors='replace').splitlines()
    timestamp, payload = lines[-1].split(' ', 1)
    diagnostic = json.loads(payload)
    result = report(snapshot, diagnostic)
    result['observerUpdatedAt'] = timestamp
    age = (datetime.datetime.now(datetime.timezone.utc) - datetime.datetime.fromisoformat(timestamp.replace('Z', '+00:00'))).total_seconds()
    result['observerStale'] = age > 35 or age < 0
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
