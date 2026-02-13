/**
 * LebensSpur - Action Cards & Config Views
 */

// ─────────────────────────────────────────────────────────────────────────────
// Config Title Mapping
// ─────────────────────────────────────────────────────────────────────────────
const configTitles = {
    'telegram': { title: 'Telegram Yapılandırması', breadcrumb: 'Aksiyonlar' },
    'early-mail': { title: 'Erken Uyarı Mail', breadcrumb: 'Aksiyonlar' },
    'early-api': { title: 'API Yapılandırması', breadcrumb: 'Aksiyonlar' },
    'trigger-mail': { title: 'Mail Grupları', breadcrumb: 'Aksiyonlar' },
    'trigger-api': { title: 'API Yapılandırması', breadcrumb: 'Aksiyonlar' },
    'relay': { title: 'Röle Yapılandırması', breadcrumb: 'Aksiyonlar' }
};

// ─────────────────────────────────────────────────────────────────────────────
// Action Cards Management
// ─────────────────────────────────────────────────────────────────────────────
function initActionCards() {
    // Action card toggle (ana icerik alanina tiklaninca)
    document.querySelectorAll('.action-card-main').forEach(main => {
        main.addEventListener('click', () => {
            const card = main.closest('.action-card');
            const isActive = card.dataset.active === 'true';
            card.dataset.active = isActive ? 'false' : 'true';
        });
    });

    // Ayarsiz kartlar icin kart geneli toggle
    document.querySelectorAll('.action-card:not(.has-settings)').forEach(card => {
        card.addEventListener('click', () => {
            const isActive = card.dataset.active === 'true';
            card.dataset.active = isActive ? 'false' : 'true';
        });
    });

    // Action config buttons (yapilandirma)
    document.querySelectorAll('.action-card-settings').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            const configType = btn.dataset.config;
            openActionConfig(configType);
        });
    });

    // Modal back button (merkezi geri butonu)
    const modalBackBtn = document.getElementById('modalBackBtn');
    if (modalBackBtn) {
        modalBackBtn.addEventListener('click', handleModalBack);
    }

    // Generic subpage handler (Sistem sekmesi icin)
    document.querySelectorAll('[data-subpage]').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const subpageId = btn.dataset.subpage;
            const subpage = document.getElementById(subpageId);
            if (!subpage) return;

            const title = subpage.dataset.title || 'Alt Sayfa';
            const breadcrumb = subpage.dataset.breadcrumb || null;

            openSystemSubpage(subpageId, title, breadcrumb);
        });
    });

    // API Config cancel/save buttons
    const apiCancelBtn = document.getElementById('apiConfigCancelBtn');
    const apiSaveBtn = document.getElementById('apiConfigSaveBtn');

    if (apiCancelBtn) apiCancelBtn.addEventListener('click', handleModalBack);
    if (apiSaveBtn) apiSaveBtn.addEventListener('click', () => {
        const webhookUrl = document.getElementById('webhookUrl')?.value || '';
        const webhookMethod = document.getElementById('webhookMethod')?.value || 'POST';
        const webhookHeaders = document.getElementById('webhookHeaders')?.value || '';
        const webhookBody = document.getElementById('webhookBody')?.value || '';
        authFetch('/api/config/webhook', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ webhookUrl, webhookMethod, webhookHeaders, webhookBody })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast(t('toast.apiSaved'), 'success');
                handleModalBack();
            } else showToast(data.error || 'Hata', 'error');
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    });

    // Relay Config cancel/save buttons
    const relayCancelBtn = document.getElementById('relayConfigCancelBtn');
    const relaySaveBtn = document.getElementById('relayConfigSaveBtn');

    if (relayCancelBtn) relayCancelBtn.addEventListener('click', handleModalBack);
    if (relaySaveBtn) relaySaveBtn.addEventListener('click', () => {
        saveRelayConfig();
    });

    // Relay test button
    const testRelayBtn = document.getElementById('testRelayBtn');
    if (testRelayBtn) {
        testRelayBtn.addEventListener('click', () => {
            testRelayBtn.disabled = true;
            testRelayBtn.textContent = 'Test ediliyor...';
            authFetch('/api/relay/test', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    if (data.success) {
                        showToast('Röle test edildi', 'success');
                    } else {
                        showToast(data.error || 'Röle testi başarısız', 'error');
                    }
                })
                .catch(() => showToast('Bağlantı hatası', 'error'))
                .finally(() => {
                    testRelayBtn.disabled = false;
                    testRelayBtn.textContent = 'Röleyi Test Et';
                });
        });
    }

    // Relay invert toggle - enerji durumu aciklamasini guncelle
    const relayInvertedToggle = document.getElementById('relayInverted');
    const relayIdleState = document.getElementById('relayIdleState');
    const relayActiveState = document.getElementById('relayActiveState');

    if (relayInvertedToggle && relayIdleState && relayActiveState) {
        relayInvertedToggle.addEventListener('change', () => {
            if (relayInvertedToggle.checked) {
                relayIdleState.textContent = 'Enerji VAR';
                relayIdleState.classList.add('relay-state-active');
                relayActiveState.textContent = 'Enerji YOK';
                relayActiveState.classList.remove('relay-state-active');
            } else {
                relayIdleState.textContent = 'Enerji YOK';
                relayIdleState.classList.remove('relay-state-active');
                relayActiveState.textContent = 'Enerji VAR';
                relayActiveState.classList.add('relay-state-active');
            }
        });
    }

    // Relay duration validation
    const relayDurationValue = document.getElementById('relayDurationValue');
    const relayDurationUnit = document.getElementById('relayDurationUnit');

    if (relayDurationValue && relayDurationUnit) {
        const validateDuration = () => {
            const unit = relayDurationUnit.value;
            let value = parseInt(relayDurationValue.value) || 1;

            if (unit === 'seconds') {
                value = Math.min(59, Math.max(1, value));
            } else {
                value = Math.min(60, Math.max(1, value));
            }

            relayDurationValue.value = value;
            updateRelayCycleInfo();
        };

        relayDurationValue.addEventListener('change', validateDuration);
        relayDurationValue.addEventListener('input', validateDuration);
        relayDurationUnit.addEventListener('change', validateDuration);
    }

    // Relay pulse toggle - pulse alanlarini goster/gizle
    const relayPulseToggle = document.getElementById('relayPulseEnabled');
    const relayPulseFields = document.getElementById('relayPulseFields');

    if (relayPulseToggle && relayPulseFields) {
        relayPulseToggle.addEventListener('change', () => {
            relayPulseFields.classList.toggle('hidden', !relayPulseToggle.checked);
            updateRelayCycleInfo();
        });
    }

    // Relay pulse duration select
    const relayPulseDuration = document.getElementById('relayPulseDuration');
    if (relayPulseDuration) {
        relayPulseDuration.addEventListener('change', updateRelayCycleInfo);
    }

    // Telegram Config cancel/save buttons
    const telegramCancelBtn = document.getElementById('telegramConfigCancelBtn');
    const telegramSaveBtn = document.getElementById('telegramConfigSaveBtn');

    if (telegramCancelBtn) telegramCancelBtn.addEventListener('click', handleModalBack);
    if (telegramSaveBtn) telegramSaveBtn.addEventListener('click', () => {
        const botToken = document.getElementById('telegramToken')?.value || '';
        const chatId = document.getElementById('telegramChat')?.value || '';
        const message = document.getElementById('telegramMessage')?.value || '';
        authFetch('/api/config/telegram', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ botToken, chatId, message })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast(t('toast.telegramSaved'), 'success');
                handleModalBack();
            } else showToast(data.error || 'Hata', 'error');
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    });

    // Early Mail Config cancel/save buttons
    const earlyMailCancelBtn = document.getElementById('earlyMailConfigCancelBtn');
    const earlyMailSaveBtn = document.getElementById('earlyMailConfigSaveBtn');

    if (earlyMailCancelBtn) earlyMailCancelBtn.addEventListener('click', handleModalBack);
    if (earlyMailSaveBtn) earlyMailSaveBtn.addEventListener('click', () => {
        const earlyRecipient = document.getElementById('earlyMailRecipient')?.value || '';
        const earlySubject = document.getElementById('earlyMailSubject')?.value || '';
        const earlyMessage = document.getElementById('earlyReminderMessage')?.value || '';
        authFetch('/api/config/early-mail', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ recipient: earlyRecipient, subject: earlySubject, message: earlyMessage })
        })
        .then(res => res.json())
        .then(data => {
            if (data.success) {
                showToast(t('toast.earlyMailSaved'), 'success');
                handleModalBack();
            } else showToast(data.error || 'Hata', 'error');
        })
        .catch(() => showToast('Bağlantı hatası', 'error'));
    });

    // Trigger Mail Config back button
    const triggerMailCancelBtn = document.getElementById('triggerMailConfigCancelBtn');
    if (triggerMailCancelBtn) triggerMailCancelBtn.addEventListener('click', handleModalBack);

    // Mail Group Detail buttons
    const mailGroupCancelBtn = document.getElementById('mailGroupCancelBtn');
    const mailGroupSaveBtn = document.getElementById('mailGroupSaveBtn');

    if (mailGroupCancelBtn) mailGroupCancelBtn.addEventListener('click', handleModalBack);
    if (mailGroupSaveBtn) mailGroupSaveBtn.addEventListener('click', () => {
        saveMailGroup();
    });

    // Mail Group list item clicks
    document.querySelectorAll('.mail-group-item [data-action="view-group"]').forEach(btn => {
        btn.addEventListener('click', (e) => {
            const groupItem = e.target.closest('.mail-group-item');
            const groupId = parseInt(groupItem.dataset.groupId);
            openMailGroupDetail(groupId);
        });
    });

    // Create Mail Group button
    const createMailGroupBtn = document.getElementById('createMailGroup');
    if (createMailGroupBtn) {
        createMailGroupBtn.addEventListener('click', () => {
            if (state.mailGroups.length >= 10) {
                showToast('Maksimum 10 mail grubu oluşturabilirsiniz', 'error');
                return;
            }
            openMailGroupDetail(null, true);
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Action Config Views
// ─────────────────────────────────────────────────────────────────────────────
function openActionConfig(type) {
    const config = configTitles[type];
    if (!config) return;

    const mainContent = document.getElementById('actionsMainContent');
    const apiView = document.getElementById('apiConfigView');
    const relayView = document.getElementById('relayConfigView');
    const telegramView = document.getElementById('telegramConfigView');
    const earlyMailView = document.getElementById('earlyMailConfigView');
    const triggerMailView = document.getElementById('triggerMailConfigView');

    if (!mainContent) return;

    // Navigation stack'e ekle
    pushView(type, config.title, config.breadcrumb);

    mainContent.classList.add('hidden');

    if ((type === 'early-api' || type === 'trigger-api') && apiView) {
        apiView.classList.remove('hidden');
    } else if (type === 'relay' && relayView) {
        relayView.classList.remove('hidden');
        loadRelayConfig();
    } else if (type === 'telegram' && telegramView) {
        telegramView.classList.remove('hidden');
    } else if (type === 'early-mail' && earlyMailView) {
        earlyMailView.classList.remove('hidden');
    } else if (type === 'trigger-mail' && triggerMailView) {
        triggerMailView.classList.remove('hidden');
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// System Subpages
// ─────────────────────────────────────────────────────────────────────────────
function openSystemSubpage(subpageId, title, breadcrumb) {
    const subpage = document.getElementById(subpageId);
    if (!subpage) return;

    const panel = subpage.closest('.settings-panel');
    if (!panel) return;

    pushView(subpageId, title, breadcrumb);

    const systemMainContent = document.getElementById('systemMainContent');
    const infoMainContent = document.getElementById('infoMainContent');

    if (panel.id === 'panel-system' && systemMainContent) {
        systemMainContent.classList.add('hidden');
        document.querySelectorAll('#panel-system .subpage').forEach(sp => {
            sp.classList.add('hidden');
        });
    } else if (panel.id === 'panel-info' && infoMainContent) {
        infoMainContent.classList.add('hidden');
        document.querySelectorAll('#panel-info .subpage').forEach(sp => {
            sp.classList.add('hidden');
        });
    }

    subpage.classList.remove('hidden');
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA Source Selector
// ─────────────────────────────────────────────────────────────────────────────
function initOtaSourceSelectors() {
    // HW OTA Kaynak Secici
    const hwOtaRadios = document.querySelectorAll('input[name="hwOtaSource"]');
    const hwOtaWarning = document.getElementById('hwOtaWarning');
    const hwOtaCustomSection = document.getElementById('hwOtaCustomSection');

    hwOtaRadios.forEach(radio => {
        radio.addEventListener('change', () => {
            const isCustom = radio.value === 'custom' && radio.checked;
            if (hwOtaWarning) hwOtaWarning.classList.toggle('hidden', !isCustom);
            if (hwOtaCustomSection) hwOtaCustomSection.classList.toggle('hidden', !isCustom);
        });
    });

    // GUI OTA Kaynak Secici
    const guiOtaRadios = document.querySelectorAll('input[name="guiOtaSource"]');
    const guiOtaWarning = document.getElementById('guiOtaWarning');
    const guiOtaCustomSection = document.getElementById('guiOtaCustomSection');

    guiOtaRadios.forEach(radio => {
        radio.addEventListener('change', () => {
            const isCustom = radio.value === 'custom' && radio.checked;
            if (guiOtaWarning) guiOtaWarning.classList.toggle('hidden', !isCustom);
            if (guiOtaCustomSection) guiOtaCustomSection.classList.toggle('hidden', !isCustom);
        });
    });
}
