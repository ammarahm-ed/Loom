// Receives report() posts from the app and appends them to a JSON-lines file.
// Usage: node scripts/collect.js <out.jsonl>
const http = require('http');
const fs = require('fs');
const out = process.argv[2] || 'results.jsonl';
http
  .createServer((req, res) => {
    let body = '';
    req.on('data', c => (body += c));
    req.on('end', () => {
      if (body) {
        fs.appendFileSync(out, body.replace(/\n/g, ' ') + '\n');
        const r = JSON.parse(body);
        console.log(`${r.pass ? 'PASS' : 'FAIL'} ${r.lib} ${JSON.stringify(r.checks)}`);
      }
      res.end('ok');
    });
  })
  .listen(8099, () => console.log('collecting on :8099 ->', out));
