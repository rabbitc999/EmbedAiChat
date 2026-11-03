"""Deterministic stdio MCP fixture; stdout is exclusively JSON-RPC."""
import json, sys, time
if "--hang" in sys.argv:
    time.sleep(60)
sys.stdin.reconfigure(encoding="utf-8")
sys.stdout.reconfigure(encoding="utf-8")
for line in sys.stdin:
    request=json.loads(line)
    method=request.get('method')
    if 'id' not in request:
        continue
    if method=='initialize':
        result={'protocolVersion':'2025-11-25','capabilities':{'tools':{}},'serverInfo':{'name':'fixture','version':'1'}}
    elif method=='tools/list':
        if request.get('params',{}).get('cursor')=='second':
            result={'tools':[{'name':'fail','description':'Return a tool-level failure','inputSchema':{'type':'object','additionalProperties':False}}]}
        else:
            result={'tools':[{'name':'echo','description':'Echo a string','inputSchema':{'type':'object','properties':{'text':{'type':'string'}},'required':['text'],'additionalProperties':False}}],'nextCursor':'second'}
    elif method=='tools/call':
        args=request['params']['arguments']
        if request['params']['name']=='fail':
            result={'isError':True,'content':[{'type':'text','text':'fixture failure'}]}
        else:
            result={'content':[{'type':'text','text':args['text']}],'structuredContent':{'echo':args['text']}}
    else:
        print(json.dumps({'jsonrpc':'2.0','id':request['id'],'error':{'code':-32601,'message':'unknown method'}}),flush=True)
        continue
    print(json.dumps({'jsonrpc':'2.0','id':request['id'],'result':result}),flush=True)
