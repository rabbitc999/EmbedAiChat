"""Run the same console acceptance tests against all implementations (no paid API calls)."""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT=Path(__file__).resolve().parents[1]
COMMANDS={}

class Handler(BaseHTTPRequestHandler):
    # 模拟模型检查真实回传的工具结果，不只返回固定聊天文字。
    errors=[]
    def log_message(self,*args): pass
    def do_POST(self):
        try:
            body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            anthropic=self.path.endswith('/messages')
            if body['model'] in ('after-edit-failure','loop'):
                completed=sum(m.get('role')=='tool' for m in body['messages'])
                if body['model']=='after-edit-failure' and completed:
                    self.send_error(503)
                    return
                call={'id':f'edit-{completed}','type':'function','function':{'name':'host_set_value','arguments':json.dumps({'value':42 if body['model']=='after-edit-failure' else completed+1})}}
                data=json.dumps({'choices':[{'message':{'role':'assistant','content':None,'tool_calls':[call]}}]}).encode()
                self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
            if body['model']=='compact':
                summary=any('Summarize this historical' in str(m.get('content')) for m in body['messages'])
                reply='compressed context' if summary else 'plain answer'
                data=json.dumps({'content':[{'type':'text','text':reply}]} if anthropic else {'choices':[{'message':{'role':'assistant','content':reply}}]}).encode()
                self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
            content=body['messages']
            had_tools=any(m.get('role')=='tool' or isinstance(m.get('content'),list) and any(x.get('type')=='tool_result' for x in m['content']) for m in content)
            if not had_tools:
                for tool in body['tools']:
                    schema=tool['input_schema'] if anthropic else tool['function']['parameters']
                    assert schema['type']=='object'
                    assert 'inputSchema' not in tool
                names={t['name'] if anthropic else t['function']['name'] for t in body['tools']}
                assert {'host_set_title','host_set_value','test_echo','test_fail','skill_activate'}<=names,names
                calls=[('host_set_title',{'title':'验证宿主'}),('host_set_value',{'value':42}),('test_echo',{'text':'MCP成功'}),('skill_activate',{'name':'demo-host'}),('test_fail',{})]
                if anthropic:
                    answer={'content':[{'type':'tool_use','id':str(i),'name':name,'input':args} for i,(name,args) in enumerate(calls)]}
                else:
                    answer={'choices':[{'message':{'role':'assistant','content':None,'tool_calls':[{'id':str(i),'type':'function','function':{'name':name,'arguments':json.dumps(args,ensure_ascii=False)}} for i,(name,args) in enumerate(calls)]}}]}
            else:
                if anthropic:
                    results=[json.loads(b['content']) for m in content if isinstance(m.get('content'),list) for b in m['content'] if b['type']=='tool_result']
                    system=body['system']
                else:
                    results=[json.loads(m['content']) for m in content if m['role']=='tool'];system=content[0]['content']
                assert len(results)==5 and all(r['ok'] for r in results[:4]) and not results[4]['ok'],results
                assert results[4]['result']['isError'] is True
                assert results[0]['result']['title']=='验证宿主'
                assert results[1]['result']['value']==42
                assert results[2]['result']['structuredContent']['echo']=='MCP成功'
                assert results[3]['result']['active']=='demo-host'
                assert 'Console demo host' in system,'Skill was not activated in next model request'
                answer={'content':[{'type':'text','text':'**验证成功**'}]} if anthropic else {'choices':[{'message':{'role':'assistant','content':'**验证成功**'}}]}
            data=json.dumps(answer,ensure_ascii=False).encode()
            self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        except Exception as error:
            self.errors.append(repr(error));self.send_error(500)

class Acceptance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        cls.thread=threading.Thread(target=cls.server.serve_forever,daemon=True);cls.thread.start()
    @classmethod
    def tearDownClass(cls): cls.server.shutdown();cls.server.server_close()
    def run_console(self,language,arguments=(),stdin=None):
        p=subprocess.run(COMMANDS[language]+list(arguments),input=stdin,encoding='utf-8',capture_output=True,timeout=35,cwd=ROOT)
        self.assertEqual(p.returncode,0,p.stderr+p.stdout)
        return p.stdout,p.stderr
    def test_model_tool_loop_both_protocols(self):
        for language in COMMANDS:
            for provider in ('openai','anthropic'):
                with self.subTest(language=language,provider=provider),tempfile.TemporaryDirectory() as directory:
                    config=Path(directory)/'config.json'
                    config.write_text(json.dumps({'provider':provider,'base_url':f'http://127.0.0.1:{self.server.server_port}/v1','model':'fixture','allow_tools':True,'skills':[str(ROOT/'examples/skills/demo-host')],'mcp':[{'prefix':'test','command':[sys.executable,str(ROOT/'tests/mock_mcp.py')]}]},ensure_ascii=False),encoding='utf-8')
                    out,err=self.run_console(language,['--config',str(config),'--ask','run acceptance'])
                    archive=json.loads(out.strip().splitlines()[-1])
                    self.assertEqual(archive['host'],{'title':'验证宿主','value':42})
                    self.assertEqual(len(archive['history']['audit']),5)
                    self.assertEqual(archive['history']['messages'][-1]['content'],'**验证成功**')
                    self.assertFalse(err,err)
        self.assertFalse(Handler.errors,Handler.errors)
    def test_allowlist_validation_and_history(self):
        for language in COMMANDS:
            with self.subTest(language=language),tempfile.TemporaryDirectory() as directory:
                history=Path(directory)/'记录.history.json'
                commands=['/call host_set_value {"value":7}','/allow on','/call host_set_value {"value":101}','/call host_set_value {"value":7,"unexpected":true}','/call host_set_value {"value":7}',f'/load-skill {ROOT / "examples/skills/demo-host"}','/skill demo-host',f'/save {history}','/clear',f'/load {history}','/host','/exit']
                out,err=self.run_console(language,stdin='\n'.join(commands)+'\n')
                self.assertIn('Tool calls',err)
                self.assertIn('maximum',err)
                self.assertIn('rejected by schema',err)
                self.assertIn('"value":7',out)
                saved=json.loads(history.read_text(encoding='utf-8'))
                self.assertEqual(saved['version'],1)
                self.assertNotIn('api_key',saved)
    def test_mcp_manual_failure_and_skill_discovery(self):
        for language in COMMANDS:
            with self.subTest(language=language):
                argv=json.dumps([sys.executable,str(ROOT/'tests/mock_mcp.py')])
                lines=[f'/mcp t {argv}','/allow on','/call t_echo {"text":"回读"}','/call t_fail {}',f'/load-skill {ROOT/"examples/skills/demo-host"}','/call skill_list {}','/call skill_activate {"name":"demo-host"}','/exit']
                out,err=self.run_console(language,stdin='\n'.join(lines)+'\n')
                self.assertFalse(err,err);self.assertIn('回读',out);self.assertIn('demo-host',out);self.assertIn('isError',out)

    def test_context_compression_and_cross_language_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory)
            config=directory/'config.json'
            config.write_text(json.dumps({'provider':'openai','base_url':f'http://127.0.0.1:{self.server.server_port}/v1','model':'compact','context_budget':3000}),encoding='utf-8')
            for language in COMMANDS:
                with self.subTest(language=language):
                    history=directory/(language+'.history.json')
                    lines=['历史数据 '*140 for _ in range(7)]+[f'/save {history}','/exit']
                    out,err=self.run_console(language,['--config',str(config)],'\n'.join(lines)+'\n')
                    self.assertFalse(err,err)
                    data=json.loads(history.read_text(encoding='utf-8'))
                    self.assertEqual(len(data['messages']),14)
                    self.assertGreater(data['covered'],0)
                    self.assertEqual(data['summary'],'compressed context')
                    for other in COMMANDS:
                        if other==language:
                            continue
                        duplicate=directory/'roundtrip.history.json'
                        _,err=self.run_console(other,stdin=f'/load {history}\n/save {duplicate}\n/exit\n')
                        self.assertFalse(err,err)
                        self.assertEqual(json.loads(duplicate.read_text(encoding='utf-8')),data)
    def test_mcp_timeout_closes_connection(self):
        import time
        with tempfile.TemporaryDirectory() as directory:
            config=Path(directory)/'config.json'
            config.write_text(json.dumps({'mcp':[{'prefix':'hang','timeout_ms':150,'command':[sys.executable,str(ROOT/'tests/mock_mcp.py'),'--hang']}]}),encoding='utf-8')
            for language in COMMANDS:
                with self.subTest(language=language):
                    begin=time.monotonic()
                    p=subprocess.run(COMMANDS[language]+['--config',str(config)],encoding='utf-8',capture_output=True,timeout=5,cwd=ROOT)
                    self.assertNotEqual(p.returncode,0)
                    self.assertLess(time.monotonic()-begin,5)
                    self.assertTrue('timed out' in p.stderr or 'timeout' in p.stderr,p.stderr)
    def test_api_key_is_not_in_archive(self):
        for language in COMMANDS:
            with self.subTest(language=language),tempfile.TemporaryDirectory() as directory:
                history=Path(directory)/'secret.history.json'
                out,err=self.run_console(language,stdin=f'/key\nFAKE-KEY-NOT-A-REAL-CREDENTIAL\n/save {history}\n/exit\n')
                self.assertFalse(err,err)
                self.assertNotIn('FAKE-KEY',out+history.read_text(encoding='utf-8'))

    def test_failed_reply_keeps_actual_audit(self):
        for language in COMMANDS:
            with self.subTest(language=language),tempfile.TemporaryDirectory() as directory:
                config=Path(directory)/'config.json'
                history=Path(directory)/'result.history.json'
                config.write_text(json.dumps({'provider':'openai','base_url':f'http://127.0.0.1:{self.server.server_port}/v1','model':'after-edit-failure','allow_tools':True}),encoding='utf-8')
                out,err=self.run_console(language,['--config',str(config)],f'edit host\n/host\n/save {history}\n/exit\n')
                self.assertIn('503',err)
                self.assertIn('"value":42',out)
                saved=json.loads(history.read_text(encoding='utf-8'))
                self.assertEqual(len(saved['audit']),1)
                self.assertEqual(saved['audit'][0]['result']['result']['value'],42)
                self.assertTrue(saved['audit'][0]['result']['ok'])
    def test_tool_round_limit_does_not_retry(self):
        for language in COMMANDS:
            with self.subTest(language=language),tempfile.TemporaryDirectory() as directory:
                config=Path(directory)/'config.json'
                history=Path(directory)/'result.history.json'
                config.write_text(json.dumps({'provider':'openai','base_url':f'http://127.0.0.1:{self.server.server_port}/v1','model':'loop','allow_tools':True,'max_rounds':8}),encoding='utf-8')
                out,err=self.run_console(language,['--config',str(config)],f'edit host\n/host\n/save {history}\n/exit\n')
                self.assertIn('round limit',err.lower())
                self.assertIn('"value":8',out)
                self.assertEqual(len(json.loads(history.read_text(encoding='utf-8'))['audit']),8)
    def test_invalid_import_is_atomic(self):
        for language in COMMANDS:
            with self.subTest(language=language),tempfile.TemporaryDirectory() as directory:
                directory=Path(directory);valid=directory/'valid.json';invalid=directory/'invalid.json';saved=directory/'saved.history.json'
                archive={'version':1,'messages':[{'role':'user','content':'你好'},{'role':'assistant','content':'**完成**'}],'summary':'','covered':0,'audit':[]}
                valid.write_text(json.dumps(archive,ensure_ascii=False),encoding='utf-8')
                invalid.write_text(json.dumps(dict(archive,messages=[],audit={})),encoding='utf-8')
                _,err=self.run_console(language,stdin=f'/load {valid}\n/load {invalid}\n/save {saved}\n/exit\n')
                self.assertIn('Error:',err)
                self.assertEqual(json.loads(saved.read_text(encoding='utf-8')),archive)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--cpp',type=Path);parser.add_argument('--java',default='java');parser.add_argument('--go',type=Path);parser.add_argument('--rust',type=Path);parser.add_argument('--csharp',type=Path);parser.add_argument('--python',action='store_true');parser.add_argument('--python-only',action='store_true');args=parser.parse_args()
    if args.python_only and any((args.cpp,args.go,args.rust,args.csharp)):
        parser.error('--python-only cannot be combined with other language executables')
    if not args.python_only:
        if args.cpp:
            COMMANDS['cpp']=[str(args.cpp.resolve())]
        COMMANDS['java']=[args.java,'-Dfile.encoding=UTF-8','-cp',str(ROOT/'java/EmbedAiChatJava.jar')+(';' if sys.platform=='win32' else ':')+str(ROOT/'java/lib/gson-2.11.0.jar'),'io.embedai.Console']
    if args.python or args.python_only:
        COMMANDS['python']=[sys.executable,'-X','utf8',str(ROOT/'python/console.py')]
    for name in ('go','rust','csharp'):
        path=getattr(args,name)
        if path:
            COMMANDS[name]=(['dotnet',str(path.resolve())] if path.suffix.lower()=='.dll' else [str(path.resolve())])
    unittest.main(argv=[sys.argv[0]],verbosity=2)
