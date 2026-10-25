#!/usr/bin/env python3
"""Jester MCP stdio bridge and command-line client (Python 3.9+, standard library)."""
import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:18530/mcp')
    parser.add_argument('--token', help='Bearer token; alternatively set JESTER_MCP_TOKEN')
    parser.add_argument('--config', help='JSON copied from Automation > Copy MCP client configuration')
    parser.add_argument('--call', help='Call one editor tool and exit (otherwise serve MCP over stdio)')
    parser.add_argument('--arguments', default='{}', help='Tool arguments as JSON')
    parser.add_argument('--arguments-file', help='Read tool arguments from a UTF-8 JSON file')
    parser.add_argument('--list', action='store_true', help='List editor tools and exit')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    url, token = args.url, args.token or os.environ.get('JESTER_MCP_TOKEN', os.environ.get('TRICKSTER_MCP_TOKEN', ''))
    if args.config:
        with open(args.config, encoding='utf-8-sig') as f:
            config = json.load(f)
        entry = config.get('mcpServers', config).get('jester', config)
        url = entry['url']
        token = entry.get('headers', {}).get('Authorization', '').removeprefix('Bearer ')
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != 'http' or parsed.hostname not in ('localhost', '127.0.0.1') or parsed.path != '/mcp':
        parser.error('Use the local Jester HTTP /mcp endpoint')
    if not token:
        parser.error('Supply --token, --config, or JESTER_MCP_TOKEN; start MCP in the editor first')
    # Local IPC must not be routed through system HTTP proxies.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    version = None

    def send(message):
        nonlocal version
        headers = {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json',
                   'Accept': 'application/json, text/event-stream'}
        params_object = message.get('params', {})
        metadata = params_object.get('_meta', {}) if isinstance(params_object, dict) else {}
        if not isinstance(metadata, dict):
            metadata = {}
        request_version = metadata.get('io.modelcontextprotocol/protocolVersion', version)
        if request_version:
            headers['MCP-Protocol-Version'] = request_version
        if request_version == '2026-07-28':
            headers['Mcp-Method'] = message['method']
            if message['method'] in ('tools/call', 'resources/read', 'prompts/get'):
                params = message.get('params', {})
                name = params.get('uri' if message['method'] == 'resources/read' else 'name', '')
                if not isinstance(name, str):
                    name = ''
                if (not name.isascii() or name != name.strip() or
                        any(ord(c) < 32 or ord(c) > 126 for c in name) or
                        (name.startswith('=?base64?') and name.endswith('?='))):
                    import base64
                    name = '=?base64?' + base64.b64encode(name.encode('utf-8')).decode('ascii') + '?='
                headers['Mcp-Name'] = name
        request = urllib.request.Request(url, json.dumps(message, ensure_ascii=False).encode('utf-8'), headers)
        try:
            with opener.open(request, timeout=args.timeout) as response:
                body = response.read()
            result = json.loads(body) if body else None
            if message.get('method') == 'initialize' and result and 'result' in result:
                version = result['result']['protocolVersion']
            return result
        except urllib.error.HTTPError as exc:
            try:
                error = json.loads(exc.read())
                if isinstance(error, dict) and error.get('jsonrpc') == '2.0' and 'error' in error:
                    return error if 'id' in message else None
            except (ValueError, OSError):
                pass
            if 'id' not in message:
                return None
            return {'jsonrpc': '2.0', 'id': message['id'], 'error': {
                'code': -32001, 'message': 'Trickster MCP HTTP error: ' + str(exc)}}
        except (urllib.error.URLError, TimeoutError, OSError, ValueError) as exc:
            if 'id' not in message:
                print('Trickster MCP: ' + str(exc), file=sys.stderr)
                return None
            return {'jsonrpc': '2.0', 'id': message['id'], 'error': {
                'code': -32001, 'message': 'Trickster MCP unavailable: ' + str(exc)}}

    if args.call or args.list:
        init = send({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2025-11-25', 'capabilities': {},
            'clientInfo': {'name': 'TricksterCLI', 'version': '0.4.0'}}})
        if not init or 'error' in init:
            print(json.dumps(init, ensure_ascii=False, indent=2)); return 1
        send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
        if args.list:
            result = send({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'})
        else:
            if args.arguments_file:
                with open(args.arguments_file, encoding='utf-8-sig') as f:
                    arguments = json.load(f)
            else:
                arguments = json.loads(args.arguments)
            result = send({'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
                           'params': {'name': args.call, 'arguments': arguments}})
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return int(not result or 'error' in result or result.get('result', {}).get('isError', False))
    for line in sys.stdin.buffer:
        if not line.strip():
            continue
        try:
            message = json.loads(line.decode('utf-8'))
            if not isinstance(message, dict):
                raise ValueError('Expected JSON-RPC object')
            result = send(message)
        except (ValueError, UnicodeError) as exc:
            result = {'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': str(exc)}}
        if result is not None:
            sys.stdout.buffer.write(json.dumps(result, ensure_ascii=False, separators=(',', ':')).encode('utf-8') + b'\n')
            sys.stdout.buffer.flush()
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
