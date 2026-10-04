// Copy buttons on every command block.
document.querySelectorAll("pre > code").forEach((code) => {
  const button = document.createElement("button");
  button.className = "copy";
  button.type = "button";
  button.textContent = "Copy";
  button.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(code.innerText.trimEnd() + "\n");
      button.textContent = "Copied";
    } catch {
      button.textContent = "Select and copy";
    }
    setTimeout(() => (button.textContent = "Copy"), 1500);
  });
  code.parentElement.appendChild(button);
});

// Download page: the AppImage has a fixed file name, so its link to
// releases/latest/download works without this; the .deb's name carries the
// version, so ask GitHub's API which file the latest release has. On any
// failure the links stay pointing at the Releases page.
const debLink = document.getElementById("deb-link");
if (debLink) {
  fetch("https://api.github.com/repos/keit/DV3000Client/releases/latest")
    .then((response) => (response.ok ? response.json() : Promise.reject(response.status)))
    .then((release) => {
      const deb = release.assets.find((asset) => /_amd64\.deb$/.test(asset.name));
      if (deb) debLink.href = deb.browser_download_url;
      document.getElementById("latest-version").textContent =
        ` \u00b7 latest release ${release.tag_name.replace(/^v/, "")}`;
    })
    .catch(() => {});
}
