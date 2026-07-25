import requests
import json
import time
import threading
import pandas as pd
import numpy as np
from datetime import datetime, timedelta
from flask import Flask, render_template_string, jsonify, send_file
from flask_cors import CORS
import math
import os
import csv
from collections import deque
import warnings
warnings.filterwarnings('ignore')

app = Flask(__name__)
CORS(app)

# ========== CONFIGURATION ==========
COPPERBELT_LAT = -12.82
COPPERBELT_LON = 28.21
ESP32_API_URL = 'https://smartmeter-isps.onrender.com/api/data'
POLL_INTERVAL = 3  # seconds
HISTORICAL_DATA_FILE = 'historical_power_data.csv'

# ========== GLOBAL DATA STORAGE ==========
system_data = {
    'battery_voltage': 0,
    'battery_current': 0,
    'battery_soc': 0,
    'inverter_voltage': 0,
    'inverter_current': 0,
    'inverter_power': 0,
    'load_voltage': 0,
    'load_current': 0,
    'load_power': 0,
    'energy_consumed_kwh': 0,
    'energy_produced_kwh': 0,
    'load1_state': 'OFF',
    'load2_state': 'OFF',
    'load1_power': 0,
    'load2_power': 0,
    'trip_state': 'NOR',
    'power_balance': 0,
    'phase': 'P1',
    'weather': {
        'current': {'temperature': 25, 'condition': 'Mild'},
        'daily': []
    },
    'sunlight': {
        'uv_index': 5,
        'sunrise': '06:00',
        'sunset': '18:00'
    },
    'predictions': {
        'today_energy': 0,
        'tomorrow_energy': 0,
        'weekly_energy': 0,
        'peak_hours': []
    },
    'recommendations': [],
    'timestamp': datetime.now().strftime('%Y-%m-%d %H:%M:%S')
}

# Store real-time data for graphs (last 60 readings)
real_time_power = deque(maxlen=60)
historical_power = []

# ========== HISTORICAL DATA MANAGEMENT ==========
def init_historical_data():
    """Generate 2 months of sample historical data"""
    global historical_power
    print("📊 Generating 2 months of historical data...")
    
    end_date = datetime.now()
    start_date = end_date - timedelta(days=60)
    
    current_date = start_date
    while current_date <= end_date:
        hour = current_date.hour
        is_weekend = current_date.weekday() >= 5
        
        # Typical daily pattern
        if 6 <= hour < 9:
            consumed = 388 + np.random.normal(0, 30)
            produced = 200 + np.random.normal(0, 50)
        elif 9 <= hour < 12:
            consumed = 554 + np.random.normal(0, 40)
            produced = 500 + np.random.normal(0, 80)
        elif 12 <= hour < 14:
            consumed = 554 + np.random.normal(0, 50)
            produced = 700 + np.random.normal(0, 60)
        elif 14 <= hour < 17:
            consumed = 554 + np.random.normal(0, 40)
            produced = 600 + np.random.normal(0, 70)
        elif 17 <= hour < 21:
            consumed = 752 + np.random.normal(0, 60)
            produced = 300 + np.random.normal(0, 50)
        else:
            consumed = 200 + np.random.normal(0, 20)
            produced = 0
        
        if is_weekend:
            consumed = consumed * 1.15
        
        historical_power.append({
            'timestamp': current_date,
            'consumed': max(0, consumed),
            'produced': max(0, produced)
        })
        
        current_date += timedelta(hours=1)
    
    save_historical_to_csv()
    print(f"✅ Generated {len(historical_power)} historical records")

def save_historical_to_csv():
    with open(HISTORICAL_DATA_FILE, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['timestamp', 'consumed_power', 'produced_power'])
        for record in historical_power:
            writer.writerow([record['timestamp'], record['consumed'], record['produced']])

# ========== PREDICTIONS ==========
def calculate_energy_predictions():
    """Calculate energy predictions based on historical data"""
    if len(historical_power) < 24:
        return {
            'today_energy': 5.5,
            'tomorrow_energy': 5.8,
            'weekly_energy': 40,
            'peak_hours': [
                {'hour': '06:00-09:00', 'factor': 1.3, 'reason': 'Morning peak'},
                {'hour': '17:00-21:00', 'factor': 1.5, 'reason': 'Evening peak'}
            ]
        }
    
    df = pd.DataFrame(historical_power)
    df['hour'] = df['timestamp'].dt.hour
    hourly_avg = df.groupby('hour')['consumed'].mean()
    
    current_hour = datetime.now().hour
    remaining_hours = 24 - current_hour
    current_avg = hourly_avg[current_hour] if current_hour in hourly_avg.index else 500
    today_energy = (current_avg * remaining_hours + system_data['energy_consumed_kwh'] * 1000) / 1000
    
    peak_hours = []
    threshold = hourly_avg.quantile(0.75)
    for hour in range(24):
        if hourly_avg[hour] > threshold:
            peak_hours.append({
                'hour': f"{hour:02d}:00-{(hour+1):02d}:00",
                'factor': round(hourly_avg[hour] / hourly_avg.mean(), 2),
                'reason': 'High demand period'
            })
    
    return {
        'today_energy': round(today_energy, 2),
        'tomorrow_energy': round(hourly_avg.mean() * 24 / 1000, 2),
        'weekly_energy': round(hourly_avg.mean() * 24 * 7 / 1000, 2),
        'peak_hours': peak_hours[:4]
    }

def generate_recommendations():
    """Generate energy saving recommendations"""
    recommendations = []
    
    load_power = system_data['load_power']
    battery_soc = system_data['battery_soc']
    current_hour = datetime.now().hour
    
    # Power based recommendations
    if load_power > 700:
        recommendations.append("⚠️ High power consumption (>{:.0f}W). Consider turning off non-essential loads.".format(load_power))
    elif load_power > 550:
        recommendations.append("📈 Moderate power consumption. Monitor during peak hours.")
    elif load_power < 300:
        recommendations.append("✅ Low power consumption. Good energy practice!")
    
    # Battery based recommendations
    if battery_soc < 30:
        recommendations.append("🔋 Battery level low ({:.0f}%). Reduce consumption to prevent outage.".format(battery_soc))
    elif battery_soc > 80:
        recommendations.append("🔋 Battery well charged ({:.0f}%). Good for evening peak hours.".format(battery_soc))
    
    # Time based recommendations
    if 17 <= current_hour <= 21:
        recommendations.append("🌙 Evening peak hour. Run heavy appliances before 5PM or after 9PM.")
    elif 11 <= current_hour <= 14:
        recommendations.append("☀️ Peak solar production time. Good for running high-power devices.")
    
    # Load specific recommendations
    if system_data['load2_state'] == 'OFF':
        recommendations.append("💡 Only essential load running. Great energy saving!")
    elif system_data['load2_state'] == 'ON':
        recommendations.append("⚠️ Both loads active. Monitor power to avoid overload.")
    
    # Default recommendations if needed
    if len(recommendations) < 2:
        recommendations.append("💡 Schedule heavy appliances during daytime for solar power.")
        recommendations.append("💡 Set geyser to 55°C for optimal energy efficiency.")
    
    return recommendations[:4]

# ========== WEATHER DATA ==========
def get_weather_data():
    """Get weather forecast"""
    url = "https://api.open-meteo.com/v1/forecast"
    params = {
        "latitude": COPPERBELT_LAT,
        "longitude": COPPERBELT_LON,
        "daily": ["temperature_2m_max", "temperature_2m_min", "sunrise", "sunset", "uv_index_max"],
        "timezone": "Africa/Lusaka",
        "forecast_days": 7
    }
    
    try:
        response = requests.get(url, params=params, timeout=10)
        if response.status_code == 200:
            data = response.json()
            daily = data['daily']
            
            system_data['weather']['daily'] = []
            for i in range(len(daily['time'])):
                system_data['weather']['daily'].append({
                    'date': daily['time'][i],
                    'temp_max': daily['temperature_2m_max'][i],
                    'temp_min': daily['temperature_2m_min'][i],
                    'sunrise': daily['sunrise'][i],
                    'sunset': daily['sunset'][i],
                    'uv_index': daily['uv_index_max'][i]
                })
            
            # Current condition
            system_data['weather']['current']['temperature'] = daily['temperature_2m_max'][0]
            if daily['temperature_2m_max'][0] > 30:
                system_data['weather']['current']['condition'] = "Hot"
            elif daily['temperature_2m_max'][0] < 15:
                system_data['weather']['current']['condition'] = "Cool"
            else:
                system_data['weather']['current']['condition'] = "Mild"
            
            # Sunlight
            system_data['sunlight'] = {
                'uv_index': daily['uv_index_max'][0],
                'sunrise': daily['sunrise'][0].split('T')[1][:5],
                'sunset': daily['sunset'][0].split('T')[1][:5]
            }
    except Exception as e:
        print(f"Weather API error: {e}")

# ========== ESP32 DATA READING ==========
def read_esp32_data():
    """Poll data from ESP32 via cloud API"""
    while True:
        try:
            response = requests.get(ESP32_API_URL, timeout=10)
            if response.status_code == 200:
                data = response.json()

                system_data['battery_voltage'] = float(data.get('battery_voltage', 0))
                system_data['battery_current'] = float(data.get('battery_current', 0))
                system_data['battery_soc'] = float(data.get('battery_soc', 0))
                system_data['inverter_voltage'] = float(data.get('inverter_voltage', 0))
                system_data['inverter_current'] = float(data.get('inverter_current', 0))
                system_data['inverter_power'] = float(data.get('inverter_power', 0))
                system_data['load_voltage'] = float(data.get('load_voltage', 0))
                system_data['load_current'] = float(data.get('load_current', 0))
                system_data['load_power'] = float(data.get('load_power', 0))

                system_data['energy_consumed_kwh'] = float(data.get('load_energy', 0))
                system_data['energy_produced_kwh'] = float(data.get('inverter_energy', 0))

                system_data['load1_state'] = data.get('relay1_state', 'OFF')
                system_data['load2_state'] = data.get('relay2_state', 'OFF')

                system_data['power_balance'] = system_data['inverter_power'] - system_data['load_power']
                system_data['load1_power'] = system_data['load_power']
                system_data['load2_power'] = 0
                system_data['trip_state'] = 'NOR'
                system_data['phase'] = 'P1'

                system_data['timestamp'] = datetime.now().strftime('%Y-%m-%d %H:%M:%S')

                real_time_power.append({
                    'timestamp': datetime.now(),
                    'consumed': system_data['load_power'],
                    'produced': system_data['inverter_power']
                })

                system_data['predictions'] = calculate_energy_predictions()
                system_data['recommendations'] = generate_recommendations()

                print(f"📊 Load: {system_data['load_power']:.1f}W | Inv: {system_data['inverter_power']:.1f}W | Bal: {system_data['power_balance']:+.1f}W | Bat: {system_data['battery_soc']:.1f}%")
            else:
                print(f"⚠️ ESP32 API returned status {response.status_code}")

        except requests.exceptions.RequestException as e:
            print(f"❌ ESP32 API Error: {e}")

        time.sleep(POLL_INTERVAL)

def update_weather_periodically():
    """Update weather every hour"""
    while True:
        get_weather_data()
        time.sleep(3600)

# ========== FLASK ROUTES ==========
HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Solar Microgrid - Smart Energy Management</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <script src="https://cdn.plot.ly/plotly-latest.min.js"></script>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 1600px; margin: 0 auto; }
        header { text-align: center; color: white; margin-bottom: 30px; }
        h1 { font-size: 2.5em; margin-bottom: 10px; }
        .subtitle { font-size: 1.1em; opacity: 0.9; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(380px, 1fr)); gap: 25px; margin-bottom: 25px; }
        .card {
            background: white;
            border-radius: 20px;
            padding: 25px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
            transition: transform 0.3s;
        }
        .card:hover { transform: translateY(-5px); }
        .card-header {
            font-size: 1.3em;
            font-weight: bold;
            color: #1a1a2e;
            margin-bottom: 20px;
            border-bottom: 3px solid #1a1a2e;
            padding-bottom: 12px;
        }
        .value { font-size: 2.2em; font-weight: bold; color: #333; margin: 10px 0; }
        .unit { font-size: 0.5em; color: #666; }
        .status {
            padding: 8px 15px;
            border-radius: 8px;
            display: inline-block;
            font-weight: bold;
        }
        .status-normal { background: #4CAF50; color: white; }
        .status-overload { background: #ff9800; color: white; }
        .status-tripped { background: #f44336; color: white; animation: blink 1s infinite; }
        @keyframes blink {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .prediction-card {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 15px;
            border-radius: 12px;
            margin: 10px 0;
        }
        .chart-container { margin: 20px 0; height: 500px; }
        .recommendation {
            background: #e3f2fd;
            padding: 12px;
            border-radius: 10px;
            margin: 10px 0;
            border-left: 4px solid #2196F3;
        }
        .peak-hour {
            display: inline-block;
            background: #ff9800;
            color: white;
            padding: 6px 12px;
            border-radius: 8px;
            margin: 5px;
            font-size: 13px;
        }
        .load-badge {
            display: inline-block;
            padding: 4px 10px;
            border-radius: 20px;
            margin: 3px;
            font-size: 12px;
            font-weight: bold;
        }
        .load-on { background: #4CAF50; color: white; }
        .load-off { background: #9e9e9e; color: white; }
        .timestamp { font-size: 0.8em; color: #666; margin-top: 20px; text-align: center; }
        footer { text-align: center; color: white; margin-top: 30px; padding: 20px; }
        .balance-positive { color: #4CAF50; }
        .balance-negative { color: #f44336; }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>☀️ Solar Powered Microgrid</h1>
            <div class="subtitle">Copperbelt University, Kitwe | Real-time Monitoring & Energy Predictions</div>
        </header>
        
        <div class="grid">
            <div class="card">
                <div class="card-header">🔋 Battery Status</div>
                <div class="value" id="battery-soc">0<span class="unit">%</span></div>
                <div>State of Charge</div>
                <div class="value" id="battery-voltage">0<span class="unit">V</span></div>
                <div>Battery Voltage</div>
                <div class="value" id="battery-current">0<span class="unit">A</span></div>
                <div>Battery Current</div>
            </div>
            
            <div class="card">
                <div class="card-header">⚡ Power Dashboard</div>
                <div class="value" id="load-power">0<span class="unit">W</span></div>
                <div>Load Power (Consumed)</div>
                <div class="value" id="inverter-power">0<span class="unit">W</span></div>
                <div>Inverter Power (Produced)</div>
                <div class="value" id="power-balance">0<span class="unit">W</span></div>
                <div>Power Balance (Produced - Consumed)</div>
                <div id="trip-status" class="status status-normal" style="margin-top: 10px;">NORMAL</div>
            </div>
            
            <div class="card">
                <div class="card-header">🔌 Load Status</div>
                <div class="value" id="load-current">0<span class="unit">A</span></div>
                <div>Total Load Current</div>
                <div style="margin: 15px 0;">
                    <span class="load-badge load-on" id="load1-badge">🔴 Load 1: ON</span>
                    <span class="load-badge load-off" id="load2-badge">🟢 Load 2: OFF</span>
                </div>
                <div id="load-powers" style="font-size: 14px; color: #666;"></div>
                <div class="value" id="energy-consumed">0.000000<span class="unit">kWh</span></div>
                <div>Total Energy Consumed</div>
                <div class="value" id="energy-produced">0.000000<span class="unit">kWh</span></div>
                <div>Total Energy Produced</div>
            </div>
        </div>
        
        <div class="grid">
            <div class="card">
                <div class="card-header">📈 Power Comparison (Real-time)</div>
                <div class="chart-container">
                    <canvas id="powerChart"></canvas>
                </div>
            </div>
            
            <div class="card">
                <div class="card-header">📊 Energy Predictions</div>
                <div class="prediction-card">
                    <div>Today's Predicted Energy</div>
                    <div class="value" id="pred-today">0<span class="unit">kWh</span></div>
                </div>
                <div class="prediction-card">
                    <div>Tomorrow's Predicted Energy</div>
                    <div class="value" id="pred-tomorrow">0<span class="unit">kWh</span></div>
                </div>
                <div class="prediction-card">
                    <div>Weekly Predicted Energy</div>
                    <div class="value" id="pred-weekly">0<span class="unit">kWh</span></div>
                </div>
                <div class="card-header" style="margin-top: 15px;">⏰ Peak Usage Hours</div>
                <div id="peak-hours"></div>
            </div>
        </div>
        
        <div class="grid">
            <div class="card">
                <div class="card-header">📊 Historical Power Trends (2 Months)</div>
                <div class="chart-container">
                    <div id="historicalChart"></div>
                </div>
            </div>
            
            <div class="card">
                <div class="card-header">💡 Energy Recommendations</div>
                <div id="recommendations"></div>
                <div class="card-header" style="margin-top: 15px;">🌤️ Weather</div>
                <div id="weather-info"></div>
                <div id="sunlight-info"></div>
            </div>
        </div>
        
        <div id="timestamp" class="timestamp">Last Update: --</div>
        <footer>
            <p>📍 Copperbelt University, Kitwe, Zambia | 📊 Data: Historical + Real-time | 🔄 Updates every second</p>
        </footer>
    </div>
    
    <script>
        let powerChart;
        
        function initChart() {
            const ctx = document.getElementById('powerChart').getContext('2d');
            powerChart = new Chart(ctx, {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [
                        {
                            label: 'Power Consumed (W)',
                            data: [],
                            borderColor: '#f44336',
                            backgroundColor: 'rgba(244, 67, 54, 0.1)',
                            borderWidth: 3,
                            tension: 0.4,
                            fill: true
                        },
                        {
                            label: 'Power Produced - Inverter (W)',
                            data: [],
                            borderColor: '#4CAF50',
                            backgroundColor: 'rgba(76, 175, 80, 0.1)',
                            borderWidth: 3,
                            tension: 0.4,
                            fill: true
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    plugins: {
                        legend: { position: 'top', labels: { font: { size: 14 } } },
                        title: { display: true, text: 'Last 60 Seconds - Real-time Comparison', font: { size: 16 } }
                    },
                    scales: { 
                        y: { 
                            beginAtZero: true, 
                            title: { display: true, text: 'Power (Watts)', font: { size: 14 } },
                            grid: { color: '#ddd' }
                        },
                        x: {
                            title: { display: true, text: 'Time (seconds ago)', font: { size: 14 } }
                        }
                    }
                }
            });
        }
        
        function fetchData() {
            fetch('/api/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('battery-soc').innerHTML = data.battery_soc.toFixed(1) + '<span class="unit">%</span>';
                    document.getElementById('battery-voltage').innerHTML = data.battery_voltage.toFixed(1) + '<span class="unit">V</span>';
                    document.getElementById('battery-current').innerHTML = data.battery_current.toFixed(2) + '<span class="unit">A</span>';
                    document.getElementById('load-power').innerHTML = data.load_power.toFixed(1) + '<span class="unit">W</span>';
                    document.getElementById('inverter-power').innerHTML = data.inverter_power.toFixed(1) + '<span class="unit">W</span>';
                    document.getElementById('load-current').innerHTML = data.load_current.toFixed(2) + '<span class="unit">A</span>';
                    
                    // Energy with 6 decimal places
                    document.getElementById('energy-consumed').innerHTML = data.energy_consumed_kwh.toFixed(6) + '<span class="unit">kWh</span>';
                    document.getElementById('energy-produced').innerHTML = data.energy_produced_kwh.toFixed(6) + '<span class="unit">kWh</span>';
                    
                    // Power balance with color
                    const balanceElement = document.getElementById('power-balance');
                    const balance = data.power_balance;
                    balanceElement.innerHTML = balance.toFixed(1) + '<span class="unit">W</span>';
                    if (balance >= 0) {
                        balanceElement.className = 'value balance-positive';
                    } else {
                        balanceElement.className = 'value balance-negative';
                    }
                    
                    // Load badges
                    document.getElementById('load1-badge').innerHTML = '🔴 Load 1: ' + data.load1_state;
                    document.getElementById('load2-badge').innerHTML = '🟢 Load 2: ' + data.load2_state;
                    document.getElementById('load1-badge').className = 'load-badge ' + (data.load1_state === 'ON' ? 'load-on' : 'load-off');
                    document.getElementById('load2-badge').className = 'load-badge ' + (data.load2_state === 'ON' ? 'load-on' : 'load-off');
                    
                    document.getElementById('load-powers').innerHTML = `L1: ${data.load1_power?.toFixed(0) || 0}W | L2: ${data.load2_power?.toFixed(0) || 0}W`;
                    
                    const tripElement = document.getElementById('trip-status');
                    tripElement.textContent = data.trip_state;
                    if (data.trip_state === 'OVL') {
                        tripElement.className = 'status status-overload';
                        tripElement.textContent = 'OVERLOAD';
                    } else if (data.trip_state === 'NOR') {
                        tripElement.className = 'status status-normal';
                        tripElement.textContent = 'NORMAL';
                    } else {
                        tripElement.className = 'status status-tripped';
                    }
                    
                    if (data.predictions) {
                        document.getElementById('pred-today').innerHTML = data.predictions.today_energy + '<span class="unit">kWh</span>';
                        document.getElementById('pred-tomorrow').innerHTML = data.predictions.tomorrow_energy + '<span class="unit">kWh</span>';
                        document.getElementById('pred-weekly').innerHTML = data.predictions.weekly_energy + '<span class="unit">kWh</span>';
                        
                        const peakContainer = document.getElementById('peak-hours');
                        if (peakContainer && data.predictions.peak_hours) {
                            peakContainer.innerHTML = data.predictions.peak_hours.map(peak => 
                                `<div class="peak-hour">${peak.hour} (${peak.factor}x) - ${peak.reason}</div>`
                            ).join('');
                        }
                    }
                    
                    if (data.recommendations) {
                        const recContainer = document.getElementById('recommendations');
                        recContainer.innerHTML = data.recommendations.map(rec => 
                            `<div class="recommendation">${rec}</div>`
                        ).join('');
                    }
                    
                    if (data.weather && data.weather.daily && data.weather.daily.length > 0) {
                        const today = data.weather.daily[0];
                        document.getElementById('weather-info').innerHTML = `
                            <div style="margin: 10px 0;">
                                <div>🌡️ Temperature: ${today.temp_max}°C / ${today.temp_min}°C</div>
                                <div>☀️ Condition: ${data.weather.current.condition}</div>
                            </div>
                        `;
                    }
                    
                    if (data.sunlight) {
                        document.getElementById('sunlight-info').innerHTML = `
                            <div>🌅 Sunrise: ${data.sunlight.sunrise}</div>
                            <div>🌇 Sunset: ${data.sunlight.sunset}</div>
                            <div>☀️ UV Index: ${data.sunlight.uv_index}</div>
                        `;
                    }
                    
                    document.getElementById('timestamp').innerHTML = 'Last Update: ' + data.timestamp;
                })
                .catch(error => console.error('Error:', error));
        }
        
        function fetchRealTimePower() {
            fetch('/api/real_time_power')
                .then(response => response.json())
                .then(data => {
                    if (powerChart && data.consumed && data.produced) {
                        const labels = data.timestamps.map((t, i) => `${60 - data.timestamps.length + i}s ago`);
                        powerChart.data.labels = labels.slice(-30);
                        powerChart.data.datasets[0].data = data.consumed.slice(-30);
                        powerChart.data.datasets[1].data = data.produced.slice(-30);
                        powerChart.update();
                    }
                })
                .catch(error => console.error('Error:', error));
        }
        
        function loadHistoricalChart() {
            fetch('/api/historical_data')
                .then(response => response.json())
                .then(data => {
                    const trace1 = {
                        x: data.timestamps,
                        y: data.consumed,
                        name: 'Power Consumed (W)',
                        type: 'scatter',
                        mode: 'lines',
                        line: { color: '#f44336', width: 3 }
                    };
                    const trace2 = {
                        x: data.timestamps,
                        y: data.produced,
                        name: 'Power Produced (W)',
                        type: 'scatter',
                        mode: 'lines',
                        line: { color: '#4CAF50', width: 3 }
                    };
                    const layout = {
                        title: '2-Month Power History (Daily Average)',
                        xaxis: { title: 'Date', tickangle: -45 },
                        yaxis: { title: 'Power (Watts)' },
                        height: 450,
                        hovermode: 'closest',
                        legend: { x: 0, y: 1, bgcolor: 'rgba(255,255,255,0.8)' }
                    };
                    Plotly.newPlot('historicalChart', [trace1, trace2], layout, { responsive: true });
                })
                .catch(error => console.error('Error:', error));
        }
        
        initChart();
        fetchData();
        fetchRealTimePower();
        loadHistoricalChart();
        setInterval(fetchData, 1000);
        setInterval(fetchRealTimePower, 2000);
    </script>
</body>
</html>
"""

@app.route('/')
def dashboard():
    return render_template_string(HTML_TEMPLATE)

@app.route('/api/data')
def get_data():
    return jsonify(system_data)

@app.route('/api/real_time_power')
def get_real_time_power():
    consumed = [p['consumed'] for p in real_time_power]
    produced = [p['produced'] for p in real_time_power]
    timestamps = [p['timestamp'].strftime('%H:%M:%S') for p in real_time_power]
    return jsonify({'consumed': consumed, 'produced': produced, 'timestamps': timestamps})

@app.route('/api/historical_data')
def get_historical_data():
    if len(historical_power) > 0:
        df = pd.DataFrame(historical_power)
        df['date'] = df['timestamp'].dt.date
        daily_avg = df.groupby('date').agg({'consumed': 'mean', 'produced': 'mean'}).reset_index()
        return jsonify({
            'timestamps': daily_avg['date'].astype(str).tolist(),
            'consumed': daily_avg['consumed'].tolist(),
            'produced': daily_avg['produced'].tolist()
        })
    return jsonify({'timestamps': [], 'consumed': [], 'produced': []})

@app.route('/download_csv')
def download_csv():
    if os.path.exists(HISTORICAL_DATA_FILE):
        return send_file(HISTORICAL_DATA_FILE, as_attachment=True)
    return "No data available", 404

# ========== MAIN ==========
if __name__ == '__main__':
    print("=" * 70)
    print("☀️ SOLAR MICROGRID ENERGY MANAGEMENT SYSTEM")
    print("=" * 70)
    print("Load Configuration:")
    print("  - Load 1: DIRECT TO GRID (388W) - Always ON")
    print("  - Load 2: Relay on PIN 4 (166W)")
    print("=" * 70)
    print(f"📡 ESP32 API: {ESP32_API_URL}")
    print(f"📍 Location: Copperbelt University, Kitwe, Zambia")
    print(f"🌐 Dashboard: http://localhost:5000")
    print("=" * 70)
    
    # Initialize historical data
    if not os.path.exists(HISTORICAL_DATA_FILE):
        init_historical_data()
    else:
        print("📂 Loading existing historical data...")
        try:
            df = pd.read_csv(HISTORICAL_DATA_FILE)
            df['timestamp'] = pd.to_datetime(df['timestamp'])
            for _, row in df.iterrows():
                historical_power.append({
                    'timestamp': row['timestamp'],
                    'consumed': row['consumed_power'],
                    'produced': row['produced_power']
                })
            print(f"✅ Loaded {len(historical_power)} historical records")
        except Exception as e:
            print(f"⚠️ Error loading historical data: {e}")
            init_historical_data()
    
    # Get initial weather
    get_weather_data()
    
    # Start threads
    esp32_thread = threading.Thread(target=read_esp32_data, daemon=True)
    esp32_thread.start()
    
    weather_thread = threading.Thread(target=update_weather_periodically, daemon=True)
    weather_thread.start()
    
    time.sleep(2)
    print("\n✅ System Ready!")
    print("🌐 Open browser: http://localhost:5000\n")
    
    app.run(host='0.0.0.0', port=5000, debug=False, use_reloader=False)