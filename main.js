document.addEventListener('DOMContentLoaded', () => {
    const file_input = document.querySelector('.file_input');
    const upload_form = document.querySelector('.upload_form');
    document.addEventListener('click', () => {
        file_input.click();
    });
    file_input.addEventListener('change', () => {
        upload_form.submit();
    });
});