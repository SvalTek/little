// WebUI runs this file when the browser requests:
//   deno_test.ts?foo=123&bar=456
//
// The script prints its HTTP response to stdout.

const query = Deno.args[0] ?? "";
const params = new URLSearchParams(query);
const foo = Number(params.get("foo") ?? 0);
const bar = Number(params.get("bar") ?? 0);

console.log(JSON.stringify({
  runtime: "deno",
  foo,
  bar,
  total: foo + bar
}));
