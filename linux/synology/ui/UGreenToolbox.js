/* DSM provides Vue and its registered v-app-* components. */
(function () {
    "use strict";
    var className = "SYNO.SDS.App.UGreenToolbox.Instance";
    SYNO.namespace("SYNO.SDS.App.UGreenToolbox");
    SYNO.SDS.App.UGreenToolbox.Instance = Vue.extend({
        render: function (h) {
            var content = window.location.protocol === "https:"
                ? h("iframe", {
                    attrs: { src: "/ugreen-toolbox/", title: "UGREEN工具箱", referrerpolicy: "same-origin" },
                    style: { width: "100%", height: "100%", border: "0", display: "block" }
                })
                : h("p", { style: { padding: "24px" } }, "请通过 HTTPS 登录 DSM 后，再打开 UGREEN工具箱。");
            return h("v-app-instance", { props: { className: className } }, [
                h("v-app-window", {
                    ref: "appWindow",
                    props: { width: 1120, height: 760, resizable: true },
                    attrs: { "syno-id": "SYNO.SDS.App.UGreenToolbox.Window" }
                }, [content])
            ]);
        },
        beforeDestroy: function () {
            if (window.location.protocol === "https:") {
                fetch("/ugreen-toolbox/session/logout", {
                    method: "POST", credentials: "same-origin", keepalive: true,
                    headers: { "X-UGREEN-Request": "1" }
                }).catch(function () {});
            }
        }
    });
}());
