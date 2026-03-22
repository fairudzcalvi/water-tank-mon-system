// Sample notification data
const notifications = [
    {
        datetime: '2024-02-10 14:30',
        type: 'Info',
        message: 'Water level normal at 65cm',
        recipient: '+639123456789',
        status: 'SENT'
    },
    {
        datetime: '2024-02-10 08:45',
        type: 'Low Alert',
        message: 'Water level critically low at 18cm',
        recipient: '+639123456789',
        status: 'SENT'
    },
    {
        datetime: '2024-02-10 08:30',
        type: 'Low Alert',
        message: 'Water level below threshold at 18cm',
        recipient: '+639123456789',
        status: 'SENT'
    },
    {
        datetime: '2024-02-09 18:20',
        type: 'Full Alert',
        message: 'Water tank nearly full at 92cm',
        recipient: '+639123456789',
        status: 'SENT'
    },
    {
        datetime: '2024-02-09 12:15',
        type: 'Info',
        message: 'Daily status report - Level: 75cm',
        recipient: '+639123456789',
        status: 'SENT'
    },
    {
        datetime: '2024-02-09 08:00',
        type: 'System',
        message: 'System startup notification',
        recipient: '+639123456789',
        status: 'SENT'
    },
    {
        datetime: '2024-02-08 16:45',
        type: 'Low Alert',
        message: 'Water level low at 22cm',
        recipient: '+639123456789',
        status: 'SENT'
    },
    {
        datetime: '2024-02-08 09:30',
        type: 'Info',
        message: 'Water level stable at 55cm',
        recipient: '+639123456789',
        status: 'SENT'
    }
];

// Get DOM elements
const modal = document.getElementById('notifications-modal');
const viewBtn = document.getElementById('view-notifications');
const closeBtn = document.getElementById('close-modal');
const notificationsList = document.getElementById('notifications-list');

// Populate notifications table
function populateNotifications() {
    notificationsList.innerHTML = '';
    notifications.forEach(notification => {
        const row = document.createElement('tr');
        row.innerHTML = `
            <td>${notification.datetime}</td>
            <td>${notification.type}</td>
            <td>${notification.message}</td>
            <td>${notification.recipient}</td>
            <td>${notification.status}</td>
        `;
        notificationsList.appendChild(row);
    });
}

// Open modal
viewBtn.addEventListener('click', () => {
    populateNotifications();
    modal.style.display = 'block';
});

// Close modal
closeBtn.addEventListener('click', () => {
    modal.style.display = 'none';
});

// Close modal when clicking outside
window.addEventListener('click', (event) => {
    if (event.target === modal) {
        modal.style.display = 'none';
    }
});

// Update notification summary
function updateNotificationSummary() {
    const today = new Date().toISOString().split('T')[0];
    const todayNotifications = notifications.filter(n => 
        n.datetime.startsWith(today.replace(/-/g, '-'))
    );
    
    document.getElementById('sms-count').textContent = todayNotifications.length;
    
    if (notifications.length > 0) {
        const lastNotification = notifications[0];
        const time = lastNotification.datetime.split(' ')[1];
        document.getElementById('last-sms').textContent = time;
    }
}

// Initialize
updateNotificationSummary();
