(() => {
  'use strict';
  window.QDCModuleEvents = class {
    constructor(base, handlers) {
      this.base = base; this.handlers = handlers; this.authenticated = false;
      this.source = null; this.retry = null; this.epoch = 0; this.delay = 10000;
      this.last = null; this.hadError = false;
      document.addEventListener('visibilitychange', () => this.visibility());
      window.addEventListener('pagehide', () => this.close());
      window.addEventListener('pageshow', () => this.visibility());
      window.addEventListener('online', () => { this.delay = 10000; this.visibility(); });
    }
    auth(value) { this.close(); this.authenticated = value; this.last = null; if (value) this.connect(); }
    visible() { return !document.hidden || this.handlers.activeCall(); }
    close() { this.epoch++; clearTimeout(this.retry); this.retry = null; this.source?.close(); this.source = null; }
    visibility() {
      if (!this.authenticated) return;
      if (!this.visible()) { this.close(); this.handlers.status('网页已休眠；后台提醒由 Bark 接收'); }
      else this.connect();
    }
    connect() {
      if (!this.authenticated || !this.visible() || this.source || this.retry) return;
      if (!window.EventSource) { this.handlers.status('浏览器不支持实时通知，请手动刷新'); return; }
      const epoch = ++this.epoch, source = new EventSource(`${this.base}/cgi-bin/api/events`);
      this.source = source;
      this.handlers.status('正在连接实时通知…');
      source.addEventListener('availability', event => {
        if (epoch !== this.epoch) return;
        try { const data = JSON.parse(event.data); this.handlers.status(data.ready ? '实时通知已连接 · 有变化时更新' : '模块监听暂不可用，可手动刷新'); } catch (_) {}
      });
      source.addEventListener('state', event => {
        if (epoch !== this.epoch) return;
        try {
          const current = JSON.parse(event.data);
          if (current.ok !== true || typeof current.epoch !== 'string' || !Number.isSafeInteger(current.smsRevision) || !Number.isSafeInteger(current.callRevision)) return;
          const previous = this.last, changedSMS = previous && previous.epoch === current.epoch && current.smsRevision !== previous.smsRevision;
          this.last = current; this.delay = 10000;
          this.handlers.state(current, !!changedSMS);
          if (!this.visible()) this.visibility();
          if (this.hadError) { this.handlers.reconnected(); this.hadError = false; }
        } catch (_) { this.handlers.status('实时通知数据无效，请手动刷新'); }
      });
      source.addEventListener('expired', () => { if (epoch === this.epoch) { this.close(); this.handlers.expired(); } });
      source.onerror = () => {
        if (epoch !== this.epoch) return;
        source.close(); this.source = null; this.hadError = true;
        this.handlers.status('实时通知中断，稍后重连；也可以手动刷新');
        /* Replace EventSource's fixed retry with bounded exponential backoff. */
        if (!this.authenticated || !this.visible()) return;
        this.retry = setTimeout(() => { this.retry = null; this.connect(); }, this.delay);
        this.delay = Math.min(this.delay * 2, 120000);
      };
    }
  };
})();
