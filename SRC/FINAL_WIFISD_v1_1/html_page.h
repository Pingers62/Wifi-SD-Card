#pragma once
const char MAIN_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>%VERSION%</title>
<style>
body { background:#111; color:#eee; font-family:Arial; margin:20px; }
.card { background:#222; padding:20px; border-radius:10px; max-width:700px; margin:auto; }
h2 { text-align:center; }

table { width:100%; border-collapse:collapse; margin-top:10px; font-size:14px; }
th,td { padding:6px; border-bottom:1px solid #444; }

button {
  padding:6px 12px;
  background:#4ea3ff;
  border:none;
  border-radius:4px;
  cursor:pointer;
  color:#000;
  font-weight:bold;
}

button:disabled {
  opacity:0.4;
  cursor:default;
}

.action-btn {
  padding:4px 12px;
  margin:0 2px;
  font-size:16px;
}

.delete-btn {
  background:#ff4e4e;
  color:#000;
}

.download-btn {
  background:#4eff7a;
  color:#000;
}

.file-label {
  padding:6px 12px;
  background:#ffe44e;
  border:none;
  border-radius:4px;
  cursor:pointer;
  color:#000;
  font-weight:bold;
  display:inline-block;
}
#fileInput { display:none; }

.upload-row {
  display:flex;
  align-items:center;
  gap:10px;
  justify-content:flex-start;
}

.upload-row .file-label { flex-shrink:0; }
.upload-row .filename {
  flex:1;
  overflow:hidden;
  white-space:nowrap;
  text-overflow:ellipsis;
  padding-left:5px;
  font-size:14px;
  color:#ccc;
}

.upload-row button { margin-right:40px; }

progress {
  width:100%;
  height:20px;
  border-radius:10px;
  overflow:hidden;
  display:none;
  margin-top:10px;
}
progress::-webkit-progress-value { background:#4ea3ff; }
progress::-webkit-progress-bar { background:#333; }
</style>
</head>
<body>
<div class="card">
<h2>%VERSION%</h2>

<!-- WEB REBOOT BUTTON -->
<div style="text-align:center; margin-top:15px;">
  <button id="rebootBtn" onclick="toggleMode()"
          style="background:#ff3b3b; color:white; padding:10px 20px;
                 border:none; border-radius:10px; font-size:16px;">
    REBOOT to USB Mode
  </button>
</div>

<!-- Upload section -->
<div class="upload-row" style="margin-top:20px;">
  <label for="fileInput" class="file-label">Choose File</label>
  <span class="filename" id="filename">No file chosen</span>
  <input type="file" id="fileInput" onchange="showFilename()">
  <button id="uploadBtn" onclick="uploadFile()">Upload</button>
</div>

<progress id="uploadProgress" value="0" max="100"></progress>
<progress id="downloadProgress" value="0" max="100"></progress>

<h3>Files on SD</h3>
<div id="filelist"></div>

</div>

<script>
let busy = false;

function setBusy(state) {
  busy = state;

  document.getElementById('rebootBtn').disabled = state;
  document.getElementById('uploadBtn').disabled = state;
  document.getElementById('fileInput').disabled = state;

  const btns = document.querySelectorAll('.action-btn');
  btns.forEach(b => b.disabled = state);
}

function toggleMode() {
  if (busy) return;
  setBusy(true);
  fetch('/toggle_mode').then(() => alert("Rebooting..."));
}

function showFilename() {
  const file = document.getElementById('fileInput').files[0];
  document.getElementById('filename').innerText = file ? file.name : "No file chosen";
}

function decodeFatDate(d) {
  if (!d) return "";
  const year = 1980 + ((d >> 9) & 0x7F);
  const month = (d >> 5) & 0x0F;
  const day = d & 0x1F;
  return `${year}-${String(month).padStart(2,'0')}-${String(day).padStart(2,'0')}`;
}

function decodeFatTime(t) {
  if (!t) return "";
  const hour = (t >> 11) & 0x1F;
  const min = (t >> 5) & 0x3F;
  const sec = (t & 0x1F) * 2;
  return `${String(hour).padStart(2,'0')}:${String(min).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;
}

async function loadFiles() {
  try {
    const res = await fetch('/list');
    if (!res.ok) {
      document.getElementById('filelist').innerText = "No files.";
      setBusy(false);
      return;
    }

    const data = await res.json();
    const files = data.files || [];

    if (!files.length) {
      document.getElementById('filelist').innerText = "No files.";
      setBusy(false);
      return;
    }

    let html = "<table><tr><th>Name</th><th>Size</th><th>Date</th><th>Time</th><th>Actions</th></tr>";
    for (const f of files) {
      const enc = encodeURIComponent(f.name);
      const dateStr = decodeFatDate(f.date || 0);
      const timeStr = decodeFatTime(f.time || 0);

      html += `<tr>
        <td>${f.name}</td>
        <td>${f.size}</td>
        <td>${dateStr}</td>
        <td>${timeStr}</td>
        <td>
          <button class="action-btn download-btn" onclick="startDownload('${enc}')">⭳</button>
          <button class="action-btn delete-btn" onclick="delFile('${enc}')">X</button>
        </td>
      </tr>`;
    }
    html += "</table>";
    document.getElementById('filelist').innerHTML = html;

  } catch (e) {
    document.getElementById('filelist').innerText = "No files.";
  }

  setBusy(false);
}

function startDownload(name) {
  if (busy) return;
  setBusy(true);

  const progress = document.getElementById('downloadProgress');
  progress.style.display = "block";
  progress.value = 0;

  const xhr = new XMLHttpRequest();
  xhr.open("GET", "/download?file=" + name, true);
  xhr.responseType = "blob";

  xhr.onprogress = (e) => {
    if (e.lengthComputable) {
      progress.value = (e.loaded / e.total) * 100;
    }
  };

  xhr.onload = () => {
    progress.style.display = "none";
    progress.value = 0;

    if (xhr.status === 200) {
      const blob = xhr.response;
      const url = window.URL.createObjectURL(blob);

      const a = document.createElement("a");
      a.href = url;
      a.download = decodeURIComponent(name);
      document.body.appendChild(a);
      a.click();
      a.remove();

      window.URL.revokeObjectURL(url);
    }

    setTimeout(() => loadFiles(), 500);
  };

  xhr.onerror = () => {
    progress.style.display = "none";
    progress.value = 0;
    setBusy(false);
  };

  xhr.send();
}

async function delFile(name) {
  if (busy) return;
  setBusy(true);

  await fetch('/delete?file=' + name);
  await loadFiles();
}

function uploadFile() {
   
  const file = document.getElementById('fileInput').files[0];
  if (!file) return;

  setBusy(true);

  const progress = document.getElementById('uploadProgress');
  progress.style.display = "block";
  progress.value = 0;

  const formData = new FormData();
  formData.append("file", file);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/upload", true);

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      progress.value = (e.loaded / e.total) * 100;
    }
  };

  xhr.onload = () => {
    progress.style.display = "none";
    progress.value = 0;

    document.getElementById('filename').innerText = "No file chosen";
    document.getElementById('fileInput').value = "";

    loadFiles();
  };

  xhr.onerror = () => setBusy(false);

  xhr.send(formData);
}

loadFiles();
</script>
</body>
</html>
)HTML";