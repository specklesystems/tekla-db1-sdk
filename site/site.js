document.querySelectorAll("[data-copy]").forEach((button) => {
  button.addEventListener("click", async () => {
    const code = document.getElementById(button.dataset.copy);
    const status = document.getElementById("copy-status");
    try {
      await navigator.clipboard.writeText(code.textContent);
      button.textContent = "Copied";
      status.textContent = "Commands copied to clipboard.";
      window.setTimeout(() => {
        button.textContent = "Copy";
      }, 2000);
    } catch {
      const selection = window.getSelection();
      const range = document.createRange();
      range.selectNodeContents(code);
      selection.removeAllRanges();
      selection.addRange(range);
      status.textContent = "Select and copy the highlighted commands.";
      button.textContent = "Selected";
      window.setTimeout(() => {
        button.textContent = "Copy";
      }, 2000);
    }
  });
});
