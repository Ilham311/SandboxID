// Terapkan tema manual tersimpan sebelum stylesheet dimuat, agar tidak ada
// kedipan tema salah (flash of wrong theme) saat halaman pertama kali dibuka.
(function () {
  try {
    const saved = localStorage.getItem('sbx-theme');
    if (saved === 'light' || saved === 'dark') {
      document.documentElement.setAttribute('data-theme', saved);
    }
  } catch (e) {}
})();
