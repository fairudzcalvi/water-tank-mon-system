const express = require('express');
const path = require('path');
const fs = require('fs');
const initSqlJs = require('sql.js');

const app = express();
app.use(express.json());
app.use(express.static('.'));

const DB_PATH = path.join(__dirname, 'water_tank.db');

let db;

// --- Init database ---
async function initDb() {
    const SQL = await initSqlJs();

    if (fs.existsSync(DB_PATH)) {
        const fileBuffer = fs.readFileSync(DB_PATH);
        db = new SQL.Database(fileBuffer);
    } else {
        db = new SQL.Database();
    }

    // Migrate: drop old tables if they have the old schema
    try {
        const cols = db.exec("PRAGMA table_info(thresholds)");
        if (cols.length) {
            const colNames = cols[0].values.map(r => r[1]);
            if (colNames.includes('low_cm')) {
                console.log('[DB] Old schema detected — dropping tables for migration...');
                db.run('DROP TABLE IF EXISTS thresholds');
                db.run('DROP TABLE IF EXISTS sensor_readings');
            }
        }
    } catch (e) { /* fresh DB, nothing to migrate */ }

    db.run(`
        CREATE TABLE IF NOT EXISTS sensor_readings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            liters      REAL NOT NULL,
            percentage  REAL NOT NULL,
            distance_cm REAL NOT NULL,
            max_liters  REAL NOT NULL,
            event_type  TEXT DEFAULT 'Level Update',
            action      TEXT DEFAULT 'Reading logged',
            status      TEXT DEFAULT 'OK',
            timestamp   TEXT NOT NULL
        )
    `);

    db.run(`
        CREATE TABLE IF NOT EXISTS notifications (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            type      TEXT NOT NULL,
            message   TEXT NOT NULL,
            recipient TEXT NOT NULL,
            status    TEXT DEFAULT 'SENT',
            timestamp TEXT NOT NULL
        )
    `);

    db.run(`
        CREATE TABLE IF NOT EXISTS thresholds (
            id       INTEGER PRIMARY KEY CHECK (id = 1),
            low_pct  REAL DEFAULT 20,
            high_pct REAL DEFAULT 90,
            phone    TEXT DEFAULT '+639123456789'
        )
    `);

    db.run(`INSERT OR IGNORE INTO thresholds (id, low_pct, high_pct, phone) VALUES (1, 20, 90, '+639123456789')`);

    saveDb();
    console.log('Database ready:', DB_PATH);
}

// --- Persist DB to disk after every write ---
function saveDb() {
    const data = db.export();
    fs.writeFileSync(DB_PATH, Buffer.from(data));
}

// --- Helpers ---
function getThresholds() {
    const res = db.exec('SELECT * FROM thresholds WHERE id = 1');
    if (!res.length) return { low_pct: 20, high_pct: 90, phone: '+639123456789' };
    const [id, low_pct, high_pct, phone] = res[0].values[0];
    return { id, low_pct, high_pct, phone };
}

function rowsToObjects(result) {
    if (!result.length) return [];
    const { columns, values } = result[0];
    return values.map(row =>
        Object.fromEntries(columns.map((col, i) => [col, row[i]]))
    );
}

// --- ESP32 posts sensor data here ---
app.post('/api/water-level', (req, res) => {
    const { liters, percentage, distance_cm, max_liters } = req.body;
    const t = getThresholds();
    const pct = parseFloat(percentage);
    const timestamp = new Date().toISOString();

    let event_type = 'Level Update';
    let action = 'Reading logged';
    let status = 'OK';

    if (pct <= t.low_pct) {
        event_type = 'Low Alert';
        action = 'Alert Logged';
        status = 'ALERT';
        db.run('INSERT INTO notifications (type, message, recipient, status, timestamp) VALUES (?,?,?,?,?)',
            ['Low Alert', `Water level critically low at ${pct.toFixed(1)}% (${parseFloat(liters).toFixed(2)} L)`, t.phone, 'SENT', timestamp]);
        console.log(`[ALERT] Low water: ${pct.toFixed(1)}%`);
    } else if (pct >= t.high_pct) {
        event_type = 'Full Alert';
        action = 'Alert Logged';
        status = 'ALERT';
        db.run('INSERT INTO notifications (type, message, recipient, status, timestamp) VALUES (?,?,?,?,?)',
            ['Full Alert', `Water tank nearly full at ${pct.toFixed(1)}% (${parseFloat(liters).toFixed(2)} L)`, t.phone, 'SENT', timestamp]);
        console.log(`[ALERT] Tank full: ${pct.toFixed(1)}%`);
    }

    db.run(
        'INSERT INTO sensor_readings (liters, percentage, distance_cm, max_liters, event_type, action, status, timestamp) VALUES (?,?,?,?,?,?,?,?)',
        [parseFloat(liters).toFixed(2), pct.toFixed(1), parseFloat(distance_cm).toFixed(1), parseFloat(max_liters).toFixed(2), event_type, action, status, timestamp]
    );

    saveDb();
    console.log(`[${new Date().toLocaleTimeString()}] ${pct.toFixed(1)}% | ${parseFloat(liters).toFixed(2)} L | dist: ${distance_cm} cm`);
    res.json({ status: 'ok' });
});

// --- Latest reading ---
app.get('/api/water-level', (_req, res) => {
    const result = db.exec('SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 1');
    const rows = rowsToObjects(result);
    res.json(rows[0] || null);
});

// --- History ---
app.get('/api/history', (_req, res) => {
    const result = db.exec('SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 100');
    res.json(rowsToObjects(result));
});

// --- Notifications ---
app.get('/api/notifications', (_req, res) => {
    const result = db.exec('SELECT * FROM notifications ORDER BY id DESC LIMIT 100');
    res.json(rowsToObjects(result));
});

// --- Get thresholds ---
app.get('/api/thresholds', (_req, res) => {
    res.json(getThresholds());
});

// --- Update thresholds ---
app.post('/api/thresholds', (req, res) => {
    const { type, threshold, phone } = req.body;
    const current = getThresholds();
    const low_pct  = type === 'low'  ? threshold : current.low_pct;
    const high_pct = type === 'high' ? threshold : current.high_pct;
    const newPhone = phone || current.phone;
    db.run('UPDATE thresholds SET low_pct = ?, high_pct = ?, phone = ? WHERE id = 1',
        [low_pct, high_pct, newPhone]);
    saveDb();
    console.log('[Thresholds updated]', { low_pct, high_pct, phone: newPhone });
    res.json({ message: `${type} threshold set to ${threshold}%` });
});

// --- Test SMS ---
app.post('/api/test-sms', (_req, res) => {
    const t = getThresholds();
    const timestamp = new Date().toISOString();
    db.run('INSERT INTO notifications (type, message, recipient, status, timestamp) VALUES (?,?,?,?,?)',
        ['System', 'Test SMS from dashboard', t.phone, 'SENT', timestamp]);
    saveDb();
    console.log(`[TEST SMS] Sent to ${t.phone}`);
    res.json({ message: `Test SMS sent to ${t.phone}` });
});

const PORT = 3000;
initDb().then(() => {
    app.listen(PORT, '0.0.0.0', () => {
        console.log(`\nServer running at http://localhost:${PORT}`);
        console.log(`Dashboard: http://localhost:${PORT}/water-tank-dashboard-lowfi.html\n`);
    });
});
