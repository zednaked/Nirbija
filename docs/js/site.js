// Optional session tape, and the itch link when it exists.
// This tree is private. The public git only hosts the page.

async function content() {
  const res = await fetch("content.json");
  if (!res.ok) throw new Error(res.status);
  return res.json();
}

function wireItch(data) {
  const url = data.itch && data.itch.url;
  document.querySelectorAll("[data-itch]").forEach((el) => {
    if (!url) {
      el.hidden = true;
      return;
    }
    el.hidden = false;
    const a = el.tagName === "A" ? el : el.querySelector("a");
    if (a) a.href = url;
  });
}

async function tape(data) {
  const host = document.getElementById("tape");
  const frame = document.querySelector("[data-tape]");
  if (!host || !frame) return;
  const id = data.video && data.video.youtube;
  if (!id) return;
  host.hidden = false;
  frame.replaceChildren();
  const iframe = document.createElement("iframe");
  iframe.src = `https://www.youtube-nocookie.com/embed/${id}`;
  iframe.title = data.video.caption || "session tape";
  iframe.allow =
    "accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture";
  iframe.allowFullscreen = true;
  frame.append(iframe);
}

content()
  .then((data) => {
    wireItch(data);
    tape(data);
  })
  .catch(() => {
    // static page only
  });
