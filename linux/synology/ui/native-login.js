(function () {
    "use strict";
    var form = document.getElementById("login-form");
    var password = document.getElementById("password");
    var message = document.getElementById("message");
    var button = form.querySelector("button");
    form.addEventListener("submit", async function (event) {
        event.preventDefault();
        button.disabled = true;
        message.textContent = "正在登录…";
        try {
            var bytes = new TextEncoder().encode("admin:" + password.value);
            password.value = "";
            var encoded = btoa(Array.from(bytes, function (value) { return String.fromCharCode(value); }).join(""));
            var response = await fetch("./session", {
                method: "POST", credentials: "same-origin", cache: "no-store",
                headers: { "Authorization": "Basic " + encoded, "X-UGREEN-Request": "1" }
            });
            var result = await response.json();
            if (!response.ok || !result.ok) throw new Error(result.error || "登录失败");
            window.location.reload();
        } catch (error) {
            message.textContent = error instanceof Error ? error.message : "连接失败，请重试";
            button.disabled = false;
            password.focus();
        }
    });
}());
