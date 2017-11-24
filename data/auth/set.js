var websock = null;

function listConfig(obj) {
	document.getElementById("ssid").value = obj.ssid;
	document.getElementById("ssidpass").value = obj.ssidpass;
	document.getElementById("mqttuser").value = obj.mqttuser;
	document.getElementById("mqttpass").value = obj.mqttpass;
	document.getElementById("relaytype").value = obj.relaytype;
}

function saveConfig() {
	var data = {};
	data.cmd = "config";
	data.ssid = document.getElementById("ssid").value;
	data.ssidpass = document.getElementById("ssidpass").value;
	data.mqttuser = document.getElementById("mqttuser").value;
	data.mqttpass = document.getElementById("mqttpass").value;
	data.relaytype = document.getElementById("relaytype").value;
	websock.send(JSON.stringify(data));
}

function start() {
	websock = new WebSocket("ws://" + window.location.hostname + "/ws");
	websock.onopen = function(evt) {
		websock.send("{\"cmd\":\"getconfig\"}");
	};
	websock.onclose = function(evt) {};
	websock.onerror = function(evt) {
		console.log(evt);
	};
	websock.onmessage = function(evt) {
    var obj = JSON.parse(evt.data);
    	if (obj.cmd === "config") {
		  listConfig(obj);
		} 
	}
}