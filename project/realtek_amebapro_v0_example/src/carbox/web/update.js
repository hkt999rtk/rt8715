// ==UserScript==
// @name         Firmware Upload Script with Progress
// @version      1.3
// @description  Upload firmware file via POST request (binary stream + progress bar)
// @match        http://192.168.43.1/*
// @grant        none
// ==/UserScript==

(function() {
    'use strict';

    console.log("🔥 Firmware Upload Script 已加载");

    // 创建上传界面
    const uploadDiv = document.createElement('div');
    uploadDiv.style.position = 'fixed';
    uploadDiv.style.top = '10px';
    uploadDiv.style.left = '10px';
    uploadDiv.style.padding = '10px';
    uploadDiv.style.background = '#fff';
    uploadDiv.style.border = '1px solid #000';
    uploadDiv.style.zIndex = '9999';
    uploadDiv.style.width = '300px';

    uploadDiv.innerHTML = `
        <input type="file" id="fwFileInput" accept=".bin" style="margin-bottom: 10px; width: 100%;">
        <br>
        <input type="text" id="fwIpInput" placeholder="设备 IP (如 192.168.43.1)" value="192.168.43.1" style="margin-bottom: 10px; width: 100%;">
        <br>
        <button id="fwUploadBtn">上传固件</button>
        <p id="fwStatus">状态: 等待上传</p>
        <progress id="fwProgressBar" value="0" max="100" style="width: 100%; display: none;"></progress>
    `;
    document.body.appendChild(uploadDiv);

    // 上传逻辑
    document.getElementById('fwUploadBtn').addEventListener('click', () => {
        console.log("⚡ 上传按钮被点击");

        const fileInput = document.getElementById('fwFileInput');
        const ipInput = document.getElementById('fwIpInput');
        const status = document.getElementById('fwStatus');
        const progressBar = document.getElementById('fwProgressBar');
        const file = fileInput.files[0];

        if (!file) {
            status.textContent = '状态: 请先选择文件';
            console.warn("❌ 没有选择文件");
            return;
        }

        if (!ipInput.value) {
            status.textContent = '状态: 请输入设备 IP';
            console.warn("❌ 没有输入 IP");
            return;
        }

        const xhr = new XMLHttpRequest();
        xhr.open('POST', `http://${ipInput.value}/test_post`, true);
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');

        // 上传进度
        xhr.upload.onprogress = (event) => {
            if (event.lengthComputable) {
                const percent = Math.round((event.loaded / event.total) * 100);
                progressBar.style.display = 'block';
                progressBar.value = percent;
                status.textContent = `状态: 上传中... ${percent}%`;
                console.log(`📤 上传进度: ${percent}%`);
            }
        };

        // 上传完成
        xhr.onload = () => {
            if (xhr.status >= 200 && xhr.status < 300) {
                status.textContent = '状态: 上传成功 ✅';
                console.log("✅ 上传成功");
            } else {
                status.textContent = `状态: 上传失败 (HTTP ${xhr.status})`;
                console.error("❌ 上传失败", xhr.status, xhr.responseText);
            }
            progressBar.style.display = 'none';
        };

        // 上传错误
        xhr.onerror = () => {
            status.textContent = '状态: 上传出错 ❌';
            console.error("⚠️ 上传出错");
            progressBar.style.display = 'none';
        };

        // 发送文件
        status.textContent = '状态: 开始上传...';
        progressBar.value = 0;
        progressBar.style.display = 'block';
        console.log("🚀 开始上传文件:", file.name, file.size, "bytes");
        xhr.send(file);
    });
})();
