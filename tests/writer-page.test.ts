import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { setImmediate } from "node:timers/promises";
import test from "node:test";
import vm from "node:vm";

const script = readFileSync(
  new URL("../libraries/SurfaceDevice/src/writer/index.html", import.meta.url),
  "utf8",
).match(/<script>([\s\S]*?)<\/script>/)![1];
const album = "https://music.apple.com/us/album/example/123456789";
const playlist = "https://music.apple.com/us/playlist/example/pl.123";
const track = album + "?i=456";
const id = "0123456789abcdef0123456789abcdef";
class Element {
  value = "";
  textContent = "";
  hidden = false;
  disabled = false;
  children: { value: string }[] = [];
  listeners = new Map<string, () => unknown>();
  onclick?: () => unknown;
  replaceChildren(...children: { value: string }[]) {
    this.children = children;
  }
  addEventListener(event: string, callback: () => unknown) {
    this.listeners.set(event, callback);
  }
}
type Status = {
  active: boolean;
  state: string;
  detail?: string;
  editor?: {
    id: number;
    url: string;
    shuffle: string;
    repeat: string;
    advanced: boolean;
  };
};
async function page(search: string, url = album, expired = false) {
  const elements = new Map<string, Element>();
  const element = (name: string) => {
    if (!elements.has(name)) elements.set(name, new Element());
    return elements.get(name)!;
  };
  const calls: {
    path: string;
    method: string;
    body: Record<string, unknown>;
  }[] = [];
  let status: Status = {
    active: false,
    state: "success",
    editor: {
      id: 7,
      url: playlist,
      shuffle: "off",
      repeat: "all",
      advanced: true,
    },
  };
  const source = (value: string) => {
    const kind = value.includes("?i=")
      ? "track"
      : value.includes("playlist")
        ? "playlist"
        : "album";
    return {
      url: value,
      kind,
      shuffle: "default",
      repeat: "default",
      choices: {
        shuffle: kind === "track" ? ["default"] : ["default", "on", "off"],
        repeat: ["default", "off", kind === "track" ? "one" : "all"],
      },
    };
  };
  const context = vm.createContext({
    document: { getElementById: element },
    location: { search },
    URLSearchParams,
    AbortController,
    Option: function (this: { value: string }, _text: string, value: string) {
      this.value = value;
    },
    setTimeout: () => 1,
    clearTimeout: () => {},
    fetch: async (path: string, options: { method: string; body?: string }) => {
      const body = JSON.parse(options.body || "{}");
      calls.push({ path, method: options.method, body });
      if (path === "/writer/drafts/" + id)
        return {
          ok: !expired,
          json: async () =>
            expired
              ? {
                  error: "draft_expired",
                  message: "Draft expired — share again, or paste a URL.",
                }
              : source(url),
        };
      if (path === "/api/source")
        return { ok: true, json: async () => source(body.url) };
      if (path === "/api/write" || path === "/api/read")
        status = {
          ...status,
          active: true,
          state: path === "/api/write" ? "armed-write" : "armed-read",
        };
      assert.ok(
        ["/api/status", "/api/read", "/api/write", "/api/activity"].includes(
          path,
        ),
        path,
      );
      return { ok: true, json: async () => status };
    },
  });
  const run = (code: string) => vm.runInContext(code, context);
  const settle = async () => {
    for (let i = 0; i < 5; i++) await setImmediate();
  };
  run(script);
  await settle();
  return {
    element,
    calls,
    run,
    settle,
    status: (value: Status) => {
      status = value;
    },
  };
}

test("shared source loads Default choices without arming or inheriting a previous card", async () => {
  for (const url of [album, playlist, track]) {
    const p = await page("?draft=" + id, url);
    assert.equal(p.element("url").value, url);
    assert.match(p.element("source").textContent, /Source: /);
    assert.equal(p.element("shuffle").value, "default");
    assert.equal(p.element("repeat").value, "default");
    assert.equal(p.run("editId"), 0);
    assert.equal(p.element("write").disabled, false);
    assert.deepEqual(
      p.calls.map((c) => c.method),
      ["GET", "GET"],
    );
    await p.run("poll()");
    assert.equal(p.element("url").value, url);
    assert.equal(
      p.calls.filter((c) => c.path.startsWith("/writer/")).length,
      1,
    );
    if (url === track) {
      assert.equal(p.element("shuffleRow").hidden, true);
      assert.deepEqual(
        p.element("repeat").children.map((c) => c.value),
        ["default", "off", "one"],
      );
      p.element("repeat").value = "one";
      await p.element("repeat").listeners.get("change")!();
    } else if (url === playlist) {
      p.element("shuffle").value = "on";
      await p.element("shuffle").listeners.get("change")!();
    }
    await p.element("write").onclick!();
    const write = p.calls.find((c) => c.path === "/api/write")!;
    assert.deepEqual(write.body, {
      url,
      shuffle: url === playlist ? "on" : "default",
      repeat: url === track ? "one" : "default",
      editId: 0,
    });
    assert.equal(p.element("editor").disabled, true);
  }
});

test("expired or malformed drafts remain useful and allow manual paste", async () => {
  for (const draft of [id, "bad/path", ""]) {
    const p = await page("?draft=" + draft, album, true);
    assert.match(p.element("error").textContent, /Draft expired — share again/);
    assert.equal(p.element("url").value, "");
    assert.equal(p.element("write").disabled, true);
    assert.equal(p.element("editor").disabled, false);
    assert.ok(p.calls.every((c) => c.method === "GET"));
    p.element("url").value = album;
    await p.run("classify()");
    assert.equal(p.element("error").textContent, "");
    assert.equal(p.element("write").disabled, false);
    assert.equal(p.run("editId"), 0);
  }
});

test("manual Read/Edit preserves hidden fields; shared drafts can deliberately read a card", async () => {
  const manual = await page("");
  assert.equal(manual.element("url").value, playlist);
  assert.equal(manual.element("advanced").hidden, false);
  assert.equal(manual.run("editId"), 7);
  manual.element("new").onclick!();
  await manual.settle();
  manual.element("url").value = album;
  await manual.run("classify()");
  await manual.run("poll()");
  assert.equal(manual.element("url").value, album);
  assert.equal(manual.run("editId"), 0);

  const shared = await page("?draft=" + id);
  await shared.element("read").onclick!();
  shared.status({
    active: false,
    state: "success",
    editor: {
      id: 8,
      url: playlist,
      shuffle: "off",
      repeat: "all",
      advanced: true,
    },
  });
  await shared.run("poll()");
  await shared.settle();
  assert.equal(shared.element("url").value, playlist);
  assert.equal(shared.element("advanced").hidden, false);
  assert.equal(shared.run("editId"), 8);
});
