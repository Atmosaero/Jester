"""Independent interoperability check; optional official MCP SDK installed under --sdk-path."""
import argparse
import asyncio
import json
import logging
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--url', required=True)
parser.add_argument('--sdk-path')
args = parser.parse_args()
token = os.environ['TRICKSTER_MCP_TOKEN']
root = Path(__file__).resolve().parents[2]
bridge = root / 'scripts/jester-mcp.py'

# The CLI uses only Python's standard library; output must remain valid JSON.
cli = subprocess.run([sys.executable, '-X', 'utf8', str(bridge), '--url', args.url,
                      '--call', 'editor_state', '--arguments', '{"nodes": false}'],
                     capture_output=True, text=True, encoding='utf-8', timeout=30)
assert cli.returncode == 0, cli.stderr + cli.stdout
assert 'result' in json.loads(cli.stdout)
print('PASS standard-library CLI')
for version, method, expected in [('2026-07-28', 'unknown/method', -32601), ('2099-01-01', 'server/discover', -32022)]:
    message = {'jsonrpc': '2.0', 'id': 77, 'method': method, 'params': {'_meta': {
        'io.modelcontextprotocol/protocolVersion': version,
        'io.modelcontextprotocol/clientInfo': {'name': 'Tests', 'version': '1'},
        'io.modelcontextprotocol/clientCapabilities': {}}}}
    response = subprocess.run([sys.executable, '-X', 'utf8', str(bridge), '--url', args.url],
        input=json.dumps(message)+'\n', capture_output=True, text=True, encoding='utf-8', timeout=30)
    assert json.loads(response.stdout)['error']['code'] == expected, response.stdout + response.stderr
print('PASS stdio protocol error forwarding')

if args.sdk_path and Path(args.sdk_path).exists():
    sys.path.insert(0, args.sdk_path)
    import httpx2
    from mcp import ClientSession
    from mcp.client.streamable_http import streamable_http_client
    from mcp.client.stdio import StdioServerParameters, stdio_client
    logging.getLogger('httpx2').setLevel(logging.WARNING)

    async def verify(session, modern):
        if modern:
            await session.discover()
            assert session.protocol_version == '2026-07-28'
        else:
            await session.initialize()
            assert session.protocol_version == '2025-11-25'
        tools = await session.list_tools()
        assert len(tools.tools) >= 29
        for tool in tools.tools:
            import jsonschema
            jsonschema.Draft202012Validator.check_schema(tool.input_schema)
        result = await session.call_tool('editor_state', {'nodes': False})
        assert not result.is_error
        assert result.structured_content['nodeCount'] >= 0
        resources = await session.list_resources()
        assert len(resources.resources) == 4
        read = await session.read_resource('trickster://editor/state')
        assert json.loads(read.contents[0].text)['nodeCount'] >= 0
        shot = await session.call_tool('screenshot', {'maxWidth': 640})
        assert not shot.is_error and shot.content[-1].type == 'image'
        invalid = await session.call_tool('timeline', {'operation': 'seek', 'frame': -1})
        assert invalid.is_error

    async def run():
        for modern in (False, True):
            async with httpx2.AsyncClient(headers={'Authorization': 'Bearer ' + token}, trust_env=False) as http:
                async with streamable_http_client(args.url, http_client=http) as streams:
                    async with ClientSession(*streams, read_timeout_seconds=20) as session:
                        await verify(session, modern)
            print('PASS official SDK HTTP ' + ('2026' if modern else '2025'))
        for modern in (False, True):
            params = StdioServerParameters(command=sys.executable,
                args=['-X', 'utf8', str(bridge), '--url', args.url],
                env={'TRICKSTER_MCP_TOKEN': token})
            async with stdio_client(params) as streams:
                async with ClientSession(*streams, read_timeout_seconds=20) as session:
                    await verify(session, modern)
            print('PASS official SDK stdio ' + ('2026' if modern else '2025'))
    asyncio.run(run())
else:
    print('Official SDK not installed; standard-library CLI check completed')
