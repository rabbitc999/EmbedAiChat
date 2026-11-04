"""七语言对话动作/多 MCP/数字样式字符串回归；仅本地 HTTP 和 stdio fixture。"""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = {
    'c': [str(ROOT/'bin/c/EmbedAiChatC.exe')],
    'cpp': [str(ROOT/'bin/cpp/EmbedAiChatCpp.exe')],
    'java': ['java', '-Dfile.encoding=UTF-8', '-cp', str(ROOT/'java/EmbedAiChatJava.jar')+';'+str(ROOT/'java/lib/gson-2.11.0.jar'), 'io.embedai.Console'],
    'go': [str(ROOT/'bin/go/EmbedAiChatGo.exe')],
    'rust': [str(ROOT/'bin/rust/EmbedAiChatRust.exe')],
    'csharp': ['dotnet', str(ROOT/'bin/csharp/EmbedAiChatCSharp.dll')],
    'python': [sys.executable, '-X', 'utf8', str(ROOT/'python/console.py')],
}

class Model(BaseHTTPRequestHandler):
    errors = []
    def log_message(self, *_): pass
    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            anthropic = self.path.endswith('/messages')
            messages = body['messages']
            results = [json.loads(b['content']) for m in messages for b in (m['content'] if isinstance(m.get('content'), list) else []) if b['type']=='tool_result'] if anthropic else [json.loads(m['content']) for m in messages if m['role']=='tool']
            if body['model']=='failure' and results:
                self.send_error(503); return
            if not results:
                names = {t['name'] if anthropic else t['function']['name'] for t in body['tools']}
                assert {'assistant_reply','assistant_end','a_echo','b_echo'} <= names
                text, calls = '开始执行', [('assistant_reply', {'text':'收到请求'}), ('a_echo', {'text':'1.0'}), ('b_echo', {'text':'001'})]
            elif len(results)==3:
                assert all(r['ok'] for r in results)
                assert results[1]['result']['structuredContent']['echo']=='1.0'
                assert results[2]['result']['structuredContent']['echo']=='001'
                text, calls = '', [('assistant_reply', {'text':'查询完成，继续修改'}), ('host_set_value', {'value':42})]
            else:
                assert len(results)==5 and results[-1]['ok']
                text, calls = '', [('assistant_reply', {'text':'处理完成'}), ('assistant_end', {}), ('host_set_value', {'value':99})]
            if anthropic:
                reply = {'content': ([{'type':'text','text':text}] if text else []) + [{'type':'tool_use','id':f'{len(results)}-{i}','name':name,'input':args} for i,(name,args) in enumerate(calls)]}
            else:
                reply = {'choices':[{'message':{'role':'assistant','content':text,'tool_calls':[{'id':f'{len(results)}-{i}','type':'function','function':{'name':name,'arguments':json.dumps(args, ensure_ascii=False)}} for i,(name,args) in enumerate(calls)]}}]}
            data = json.dumps(reply, ensure_ascii=False).encode()
            self.send_response(200); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
        except Exception as exc:
            self.errors.append(repr(exc)); self.send_error(500)

class ConversationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server=ThreadingHTTPServer(('127.0.0.1',0),Model)
        threading.Thread(target=cls.server.serve_forever,daemon=True).start()
    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown(); cls.server.server_close()
    def config(self, directory, provider, model='conversation'):
        config=Path(directory)/'config.json'
        config.write_text(json.dumps({'provider':provider,'model':model,'base_url':f'http://127.0.0.1:{self.server.server_port}/v1','allow_tools':True,'mcp':[{'prefix':p,'command':[sys.executable,str(ROOT/'tests/mock_mcp.py')]} for p in ('a','b')]}),encoding='utf-8')
        return config
    def run_console(self, language, args, stdin=None):
        p=subprocess.run(COMMANDS[language]+args,input=stdin,capture_output=True,encoding='utf-8',timeout=40,cwd=ROOT)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        return p.stdout,p.stderr
    def test_multiple_messages_multiple_mcp_and_explicit_end(self):
        for language in COMMANDS:
            for provider in ('openai','anthropic'):
                with self.subTest(language=language,provider=provider), tempfile.TemporaryDirectory() as directory:
                    out,err=self.run_console(language,['--config',str(self.config(directory,provider)),'--ask','run task'])
                    self.assertFalse(err,err)
                    result=json.loads(out.strip().splitlines()[-1]); history=result['history']; audit=history['audit']
                    self.assertEqual(result['host']['value'],42)
                    self.assertEqual(len(audit),8)
                    self.assertEqual([a['name'] for a in audit],['assistant_reply','a_echo','b_echo','assistant_reply','host_set_value','assistant_reply','assistant_end','host_set_value'])
                    self.assertTrue(all(a['result']['ok'] for a in audit[:-1]))
                    self.assertFalse(audit[-1]['result']['ok'])
                    self.assertEqual([m['content'] for m in history['messages'][1:]],['开始执行','收到请求','查询完成，继续修改','处理完成'])
                    self.assertEqual(out.strip().splitlines()[:-1].count('处理完成'),1)
        self.assertFalse(Model.errors,Model.errors)
    def test_failure_keeps_delivered_messages_and_audit(self):
        for language in COMMANDS:
            for provider in ('openai','anthropic'):
                with self.subTest(language=language,provider=provider),tempfile.TemporaryDirectory() as directory:
                    saved=Path(directory)/'saved.json'
                    out,err=self.run_console(language,['--config',str(self.config(directory,provider,'failure'))],f'执行任务\n/save {saved}\n/exit\n')
                    self.assertTrue(err, 'Model failure must be reported')
                    history=json.loads(saved.read_text(encoding='utf-8'))
                    self.assertEqual([m['content'] for m in history['messages'][1:]],['开始执行','收到请求'])
                    self.assertEqual(len(history['audit']),3)
                    self.assertIn('收到请求',out)
    def test_numeric_looking_strings_are_not_numbers(self):
        for language in COMMANDS:
            with self.subTest(language=language), tempfile.TemporaryDirectory() as directory:
                commands=[]
                for value in ['1.0','001','1e3','-0.0']:
                    commands.append('/call assistant_reply '+json.dumps({'text':value}))
                commands.extend(['/call assistant_reply {"text":1.0}', '/call host_set_value {"value":"1.0"}', '/host','/exit'])
                out,err=self.run_console(language,['--config',str(self.config(directory,'openai'))],'\n'.join(commands)+'\n')
                for value in ['1.0','001','1e3','-0.0']:
                    self.assertIn(json.dumps(value),out)
                self.assertTrue(err, 'Numeric/string schema mismatch must be rejected')
                # 禁止把字符串数字用于数值工具，宿主状态保持初始值。
                self.assertIn('"value":0',out)

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--languages',nargs='+',choices=COMMANDS); args=parser.parse_args()
    if args.languages: COMMANDS={k:COMMANDS[k] for k in args.languages}
    unittest.main(argv=[sys.argv[0]],verbosity=2)
