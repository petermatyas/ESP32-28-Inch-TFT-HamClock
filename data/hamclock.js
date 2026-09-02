/* ---------------------------------------------------------------------------
   Injects the same navigation bar into every HamClock page, so the list of
   pages lives in one place instead of being copied into each of them.
   --------------------------------------------------------------------------- */
(function () {
    var PAGES = [
        { href: '/',             label: 'General' },
        { href: '/clock.html',   label: 'Clock' },
        { href: '/bigclock.html', label: 'Big Clock' },
        { href: '/weather.html', label: 'Weather' },
        { href: '/sat.html',    label: 'Satellites' },
        { href: '/dx.html',     label: 'DX Cluster' },
        { href: '/wifi.html',   label: 'WiFi' }
    ];

    function currentPath() {
        var p = (location.pathname || '/').toLowerCase();
        return (p === '' || p === '/index.html') ? '/' : p;
    }

    function build() {
        if (document.querySelector('.hc-nav')) return;   // already there

        var here = currentPath();
        var nav = document.createElement('nav');
        nav.className = 'hc-nav';

        var brand = document.createElement('span');
        brand.className = 'hc-brand';
        brand.textContent = 'HamClock';
        nav.appendChild(brand);

        PAGES.forEach(function (page) {
            var a = document.createElement('a');
            a.href = page.href;
            a.textContent = page.label;
            if (page.href === here) a.className = 'active';
            nav.appendChild(a);
        });

        document.body.insertBefore(nav, document.body.firstChild);
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', build);
    } else {
        build();
    }
})();
