from flask import Flask, render_template, request, redirect
import os
import subprocess
import shutil

app = Flask(__name__, static_folder='static', template_folder='templates')

@app.route("/", methods=["GET"])
def index():
    print("Rendering index page")
    return render_template("index.html")

@app.route("/get-wifis", methods=["GET"])
def get_wifis(): 
    try:
        # Ensure nmcli is available before attempting scan
        if shutil.which("nmcli") is None:
            print("nmcli not found on system")
            return render_template("index.html", wifis=[], error="nmcli not found")

        result = subprocess.run(
            ["bash", "./wifi-manager-nmcli.sh", "search-wifi"],
            capture_output=True,
            text=True,
            check=True
        )
        wifi_list = [line for line in result.stdout.strip().splitlines() if line]
        print(f"Found WiFi networks: {wifi_list}")
        return render_template("index.html", wifis=wifi_list)
    except subprocess.CalledProcessError as e:
        print(f"Error scanning WiFi networks: {e.stderr}")
        return render_template("index.html", wifis=[])
    

@app.route("/connect", methods=["POST"])
def connect():
    
    ssid = request.form.get("ssid")
    password = request.form.get("password")

    print(f"Received WiFi credentials - SSID: {ssid}")

    if not ssid or not password:
        return "Missing WiFi credentials", 400
    
    try:
        # Use wifi-manager script to add and connect to WiFi
        if shutil.which("bash") is None:
            return "Bash not found on system", 500
        if shutil.which("nmcli") is None:
            return "nmcli not found on system", 500
        result = subprocess.run(
            ["bash", "./wifi-manager-nmcli.sh", "add-wifi", ssid, password],
            capture_output=True,
            text=True,
            check=True
        )
        start_wifi = subprocess.run(
            ["bash", "./wifi-manager-nmcli.sh", "connect-wifi", ssid],
            capture_output=True,
            text=True,
            check=True
        )
        print(f"WiFi connection result: {start_wifi.stdout}")

        subprocess.run(["captive-portal-control.sh", "stop"], check=False)
        subprocess.run(["nmcli", "connection", "down", "Rooted-Robotics-Setup"], check=False)
        subprocess.run(["nmcli", "connection", "up", ssid], check=False)
        
        return "✅ WiFi configuration saved. Connecting to network..."
    except subprocess.CalledProcessError as e:
        return f"❌ Error: {e.stderr}", 500
    return render_template("success.html", ssid=ssid)
# CAPTIVE PORTAL DETECTION - This is the key addition!
@app.route('/<path:path>')
def catch_all(path):
    """Redirect captive portal detection URLs to the portal page"""
    # List of URLs that devices check for internet connectivity
    detection_paths = ['generate_204', 'hotspot-detect', 'connecttest', 'ncsi', 'success']
    
    # If it's a detection URL, redirect to home to trigger captive portal
    if any(detect in path for detect in detection_paths):
        return redirect("/", code=302)
    
    # For all other unknown paths, also redirect to home
    return redirect("/", code=302)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=80, debug=False)