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

// Download page: the AppImages have fixed file names, so their links to
// releases/latest/download work without this; the .debs' names carry the
// version, so ask GitHub's API which files the latest release has. Any
// .deb not found, or any failure, leaves its link on the Releases page.
const debLinks = document.querySelectorAll("[data-deb-arch]");
if (debLinks.length) {
  fetch("https://api.github.com/repos/keit/DV3000Client/releases/latest")
    .then((response) => (response.ok ? response.json() : Promise.reject(response.status)))
    .then((release) => {
      debLinks.forEach((link) => {
        const suffix = `_${link.dataset.debArch}.deb`;
        const deb = release.assets.find((asset) => asset.name.endsWith(suffix));
        if (deb) link.href = deb.browser_download_url;
      });
      document.getElementById("latest-version").textContent =
        ` \u00b7 latest release ${release.tag_name.replace(/^v/, "")}`;
    })
    .catch(() => {});
}
