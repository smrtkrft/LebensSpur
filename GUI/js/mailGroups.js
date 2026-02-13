/**
 * LebensSpur - Mail Group CRUD & Recipients
 */

// ─────────────────────────────────────────────────────────────────────────────
// Mail Group Load / Persist
// ─────────────────────────────────────────────────────────────────────────────
function loadMailGroups() {
    authFetch('/api/config/mail-groups')
        .then(res => res.json())
        .then(data => {
            if (data.groups && Array.isArray(data.groups)) {
                state.mailGroups = data.groups.map((g, i) => ({
                    id: i + 1,
                    name: g.name || '',
                    subject: g.subject || '',
                    content: g.content || '',
                    files: [],
                    recipients: g.recipients || []
                }));
                renderMailGroupList();
            }
        })
        .catch(err => console.error('Mail groups load failed:', err));
}

function persistMailGroups() {
    const groups = state.mailGroups.map(g => ({
        name: g.name,
        subject: g.subject,
        content: g.content,
        recipients: g.recipients
    }));
    authFetch('/api/config/mail-groups', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ groups })
    }).catch(err => console.error('Mail group save failed:', err));
}

// ─────────────────────────────────────────────────────────────────────────────
// Mail Group List Rendering
// ─────────────────────────────────────────────────────────────────────────────
function renderMailGroupList() {
    const listEl = document.getElementById('mailGroupList');
    if (!listEl) return;

    listEl.innerHTML = state.mailGroups.map(group => `
        <div class="mail-group-item" data-group-id="${group.id}">
            <div class="mail-group-info">
                <span class="mail-group-name">${group.name}</span>
                <span class="mail-group-count">${group.recipients.length} alıcı</span>
            </div>
            <button class="btn btn-ghost btn-sm" data-action="view-group">Görüntüle</button>
        </div>
    `).join('');

    listEl.querySelectorAll('[data-action="view-group"]').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const groupItem = e.target.closest('.mail-group-item');
            const groupId = parseInt(groupItem.dataset.groupId);
            openMailGroupDetail(groupId);
        });
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Mail Group Save / Delete
// ─────────────────────────────────────────────────────────────────────────────
function saveMailGroup() {
    const name = document.getElementById('mailGroupName').value.trim();
    const subject = document.getElementById('mailGroupSubject').value.trim();
    const content = document.getElementById('mailGroupContent').value.trim();

    if (!name) {
        showToast('Grup adı gereklidir', 'error');
        return;
    }

    const recipientItems = document.querySelectorAll('#mailGroupRecipients .recipient-item');
    const recipients = Array.from(recipientItems).map(item =>
        item.querySelector('.recipient-email').textContent
    );

    const fileItems = document.querySelectorAll('#mailGroupFileList .file-item');
    const files = Array.from(fileItems).map(item =>
        item.querySelector('.file-item-name').textContent
    );

    if (state.isCreatingNewGroup) {
        const newId = Math.max(0, ...state.mailGroups.map(g => g.id)) + 1;
        const newGroup = { id: newId, name, subject, content, files, recipients };
        state.mailGroups.push(newGroup);
        showToast(t('toast.mailGroupCreated'), 'success');
    } else {
        const group = state.mailGroups.find(g => g.id === state.currentEditingGroup);
        if (group) {
            group.name = name;
            group.subject = subject;
            group.content = content;
            group.files = files;
            group.recipients = recipients;
            showToast(t('toast.mailGroupUpdated'), 'success');
        }
    }

    persistMailGroups();
    renderMailGroupList();
    handleModalBack();
}

function deleteMailGroup() {
    if (!state.currentEditingGroup) return;

    const groupIndex = state.mailGroups.findIndex(g => g.id === state.currentEditingGroup);
    if (groupIndex > -1) {
        state.mailGroups.splice(groupIndex, 1);
        persistMailGroups();
        showToast(t('toast.mailGroupDeleted'), 'success');
        renderMailGroupList();
        handleModalBack();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mail Group Detail View
// ─────────────────────────────────────────────────────────────────────────────
function openMailGroupDetail(groupIdOrName, isNew = false) {
    const triggerMailView = document.getElementById('triggerMailConfigView');
    const mailGroupDetailView = document.getElementById('mailGroupDetailView');
    const deleteBtn = document.getElementById('mailGroupDeleteBtn');

    if (!triggerMailView || !mailGroupDetailView) return;

    state.isCreatingNewGroup = isNew;

    let groupName = 'Yeni Mail Grubu';
    if (groupIdOrName && typeof groupIdOrName === 'number') {
        const group = state.mailGroups.find(g => g.id === groupIdOrName);
        if (group) {
            groupName = group.name;
            state.currentEditingGroup = groupIdOrName;
            document.getElementById('mailGroupName').value = group.name;
            document.getElementById('mailGroupSubject').value = group.subject;
            document.getElementById('mailGroupContent').value = group.content;
            renderFiles(group.files || []);
            renderRecipients(group.recipients || []);
        }
    } else if (isNew || groupIdOrName === null) {
        state.currentEditingGroup = null;
        state.isCreatingNewGroup = true;
        document.getElementById('mailGroupName').value = '';
        document.getElementById('mailGroupSubject').value = '';
        document.getElementById('mailGroupContent').value = '';
        const fileListEl = document.getElementById('mailGroupFileList');
        if (fileListEl) fileListEl.innerHTML = '';
        renderRecipients([]);
    } else if (typeof groupIdOrName === 'string') {
        groupName = groupIdOrName;
    }

    if (deleteBtn) {
        deleteBtn.style.display = isNew ? 'none' : 'flex';
    }

    const title = isNew ? 'Yeni Mail Grubu' : `${groupName} - Düzenle`;
    pushView('mail-group-detail', title, 'Mail Grupları');

    triggerMailView.classList.add('hidden');
    mailGroupDetailView.classList.remove('hidden');
}

// ─────────────────────────────────────────────────────────────────────────────
// Recipients
// ─────────────────────────────────────────────────────────────────────────────
function renderRecipients(recipients) {
    const listEl = document.getElementById('mailGroupRecipients');
    if (!listEl) return;

    listEl.innerHTML = recipients.map(email => `
        <div class="recipient-item">
            <div class="recipient-info">
                <span class="recipient-email">${email}</span>
            </div>
            <button class="btn btn-ghost btn-sm btn-delete" data-email="${email}">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16">
                    <line x1="18" y1="6" x2="6" y2="18"></line>
                    <line x1="6" y1="6" x2="18" y2="18"></line>
                </svg>
            </button>
        </div>
    `).join('');

    listEl.querySelectorAll('.btn-delete').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const email = e.currentTarget.dataset.email;
            removeRecipient(email);
        });
    });
}

function addRecipient() {
    const input = document.getElementById('newRecipientEmail');
    const email = input.value.trim();

    if (!email || !email.includes('@')) {
        showToast('Geçerli bir email adresi girin', 'error');
        return;
    }

    const recipientItems = document.querySelectorAll('#mailGroupRecipients .recipient-item');
    const currentRecipients = Array.from(recipientItems).map(item =>
        item.querySelector('.recipient-email').textContent
    );

    if (currentRecipients.includes(email)) {
        showToast('Bu email zaten ekli', 'error');
        return;
    }

    currentRecipients.push(email);
    renderRecipients(currentRecipients);
    input.value = '';
}

function removeRecipient(email) {
    const recipientItems = document.querySelectorAll('#mailGroupRecipients .recipient-item');
    const currentRecipients = Array.from(recipientItems)
        .map(item => item.querySelector('.recipient-email').textContent)
        .filter(e => e !== email);

    renderRecipients(currentRecipients);
}

// ─────────────────────────────────────────────────────────────────────────────
// File Attachments
// ─────────────────────────────────────────────────────────────────────────────
function renderFiles(files) {
    const listEl = document.getElementById('mailGroupFileList');
    if (!listEl) return;

    listEl.innerHTML = files.map(file => `
        <div class="file-item">
            <div class="file-item-info">
                <svg class="file-item-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16">
                    <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"></path>
                    <polyline points="14 2 14 8 20 8"></polyline>
                </svg>
                <span class="file-item-name">${file}</span>
            </div>
            <button class="btn btn-ghost btn-sm btn-delete" data-file="${file}">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="16" height="16">
                    <line x1="18" y1="6" x2="6" y2="18"></line>
                    <line x1="6" y1="6" x2="18" y2="18"></line>
                </svg>
            </button>
        </div>
    `).join('');

    listEl.querySelectorAll('.btn-delete').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const filename = e.currentTarget.dataset.file;
            removeFile(filename);
        });
    });
}

function handleFileUpload(e) {
    const files = e.target.files;
    if (!files.length) return;

    const fileItems = document.querySelectorAll('#mailGroupFileList .file-item');
    const currentFiles = Array.from(fileItems).map(item =>
        item.querySelector('.file-item-name').textContent
    );

    Array.from(files).forEach(file => {
        if (!currentFiles.includes(file.name)) {
            currentFiles.push(file.name);
        }
    });

    renderFiles(currentFiles);
    e.target.value = '';
}

function removeFile(filename) {
    const fileItems = document.querySelectorAll('#mailGroupFileList .file-item');
    const currentFiles = Array.from(fileItems)
        .map(item => item.querySelector('.file-item-name').textContent)
        .filter(f => f !== filename);

    renderFiles(currentFiles);
}
