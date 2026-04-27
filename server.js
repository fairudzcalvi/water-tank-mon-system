const express = require('express');
const { Pool } = require('pg');

const path = require('path');

const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname)));

// --- Supabase PostgreSQL connection ---
// Set this in your Render environment variables as DATABASE_URL
const pool = new Pool({
    connectionString: process.env.DATABASE_URL,
    ssl: { rejectUnauthorized: false }
});

// --- Helpers ---
async function getThresholds() {
    const res = await pool.query('SELECT * FROM thresholds WHERE id = 1');
    return res.rows[0] || { low_pct: 20, high_pct: 90, phone: '+639123456789' };
}

// --- Redirect root to dashboard ---
app.get('/', (_req, res) => {
    res.sendFile(path.join(__dirname, 'water-tank-dashboard-lowfi.html'));
});

// --- ESP32 posts sensor data here ---
app.post('/api/water-level', async (req, res) => {
    const { liters, percentage, distance_cm, max_liters } = req.body;
    if (liters == null || percentage == null || distance_cm == null || max_liters == null) {
        return res.status(400).json({ error: 'Missing required fields' });
    }

    const t = await getThresholds();
    const pct = parseFloat(percentage);
    const timestamp = new Date().toISOString();

    let event_type = 'Level Update';
    let action = 'Reading logged';
    let status = 'OK';

    if (pct <= t.low_pct) {
        event_type = 'Low Alert'; action = 'Alert Logged'; status = 'ALERT';
        await pool.query(
            'INSERT INTO notifications (type, message, recipient, status, timestamp) VALUES ($1,$2,$3,$4,$5)',
            ['Low Alert', `Water level critically low at ${pct.toFixed(1)}% (${parseFloat(liters).toFixed(2)} L)`, t.phone, 'SENT', timestamp]
        );
        console.log(`[ALERT] Low water: ${pct.toFixed(1)}%`);
    } else if (pct >= t.high_pct) {
        event_type = 'Full Alert'; action = 'Alert Logged'; status = 'ALERT';
        await pool.query(
            'INSERT INTO notifications (type, message, recipient, status, timestamp) VALUES ($1,$2,$3,$4,$5)',
            ['Full Alert', `Water tank nearly full at ${pct.toFixed(1)}% (${parseFloat(liters).toFixed(2)} L)`, t.phone, 'SENT', timestamp]
        );
        console.log(`[ALERT] Tank full: ${pct.toFixed(1)}%`);
    }

    await pool.query(
        'INSERT INTO sensor_readings (liters, percentage, distance_cm, max_liters, event_type, action, status, timestamp) VALUES ($1,$2,$3,$4,$5,$6,$7,$8)',
        [parseFloat(liters).toFixed(2), pct.toFixed(1), parseFloat(distance_cm).toFixed(1), parseFloat(max_liters).toFixed(2), event_type, action, status, timestamp]
    );

    console.log(`[${new Date().toLocaleTimeString()}] ${pct.toFixed(1)}% | ${parseFloat(liters).toFixed(2)} L | dist: ${distance_cm} cm`);
    res.json({ status: 'ok' });
});

// --- Latest reading ---
app.get('/api/water-level', async (_req, res) => {
    const result = await pool.query('SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 1');
    res.json(result.rows[0] || null);
});

// --- History ---
app.get('/api/history', async (_req, res) => {
    const result = await pool.query('SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 100');
    res.json(result.rows);
});

// --- Notifications ---
app.get('/api/notifications', async (_req, res) => {
    const result = await pool.query('SELECT * FROM notifications ORDER BY id DESC LIMIT 100');
    res.json(result.rows);
});

// --- Get thresholds ---
app.get('/api/thresholds', async (_req, res) => {
    res.json(await getThresholds());
});

// --- Update thresholds ---
app.post('/api/thresholds', async (req, res) => {
    const { type, threshold, phone } = req.body;
    const current = await getThresholds();
    const low_pct  = type === 'low'  ? threshold : current.low_pct;
    const high_pct = type === 'high' ? threshold : current.high_pct;
    const newPhone = phone || current.phone;
    await pool.query(
        'UPDATE thresholds SET low_pct = $1, high_pct = $2, phone = $3 WHERE id = 1',
        [low_pct, high_pct, newPhone]
    );
    console.log('[Thresholds updated]', { low_pct, high_pct, phone: newPhone });
    res.json({ message: `${type} threshold set to ${threshold}%` });
});

// --- Test SMS ---
app.post('/api/test-sms', async (_req, res) => {
    const t = await getThresholds();
    const timestamp = new Date().toISOString();
    await pool.query(
        'INSERT INTO notifications (type, message, recipient, status, timestamp) VALUES ($1,$2,$3,$4,$5)',
        ['System', 'Test SMS from dashboard', t.phone, 'SENT', timestamp]
    );
    console.log(`[TEST SMS] Sent to ${t.phone}`);
    res.json({ message: `Test SMS sent to ${t.phone}` });
});

// --- SMS Status ---
app.post('/api/sms-status', async (req, res) => {
    const { type, recipient, message, status, error } = req.body;
    const timestamp = new Date().toISOString();
    const s = status || (error ? 'FAILED' : 'SENT');
    await pool.query(
        'INSERT INTO notifications (type, message, recipient, status, timestamp) VALUES ($1,$2,$3,$4,$5)',
        [type || 'SMS', message || 'SMS delivery', recipient, s, timestamp]
    );
    await pool.query(
        'INSERT INTO sms_log (type, recipient, message, status, error, timestamp) VALUES ($1,$2,$3,$4,$5,$6)',
        [type || 'SMS', recipient, message, s, error || null, timestamp]
    );
    res.json({ status: 'logged' });
});

// --- Get SMS Log ---
app.get('/api/sms-log', async (_req, res) => {
    const result = await pool.query('SELECT * FROM sms_log ORDER BY id DESC LIMIT 50');
    res.json(result.rows);
});

// --- ESP32 posts GSM signal ---
app.post('/api/gsm-signal', async (req, res) => {
    const { rssi, quality } = req.body;
    const timestamp = new Date().toISOString();
    await pool.query(
        'INSERT INTO gsm_status (rssi, quality, timestamp) VALUES ($1,$2,$3)',
        [rssi, quality, timestamp]
    );
    res.json({ status: 'ok' });
});

// --- Get Latest GSM Signal ---
app.get('/api/gsm-signal', async (_req, res) => {
    const result = await pool.query('SELECT * FROM gsm_status ORDER BY id DESC LIMIT 1');
    res.json(result.rows[0] || { rssi: -1, quality: 'Unknown' });
});

// --- Start server ---
const PORT = process.env.PORT || 3000;
app.listen(PORT, '0.0.0.0', () => {
    console.log(`\nServer running at http://localhost:${PORT}`);
    console.log(`Dashboard: http://localhost:${PORT}/water-tank-dashboard-lowfi.html\n`);
});
