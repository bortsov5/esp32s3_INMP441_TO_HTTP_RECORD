<?php
// upl_simple.php
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: POST');
header('Access-Control-Allow-Headers: Content-Type');

// Создаем директорию если нет
if (!file_exists('uploads')) {
    mkdir('uploads', 0777, true);
}

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    // Получаем сырые данные
    $input = file_get_contents('php://input');
    
    if (empty($input)) {
        http_response_code(400);
        echo "Ошибка: нет данных";
        return;
    }
    
    // Ищем самый новый WAV файл
    $newestFile = null;
    $newestTime = 0;
    
    $files = scandir('uploads');
    foreach ($files as $file) {
        if (pathinfo($file, PATHINFO_EXTENSION) === 'wav') {
            $filepath = "uploads/{$file}";
            $modTime = filemtime($filepath);
            
            if ($modTime > $newestTime) {
                $newestTime = $modTime;
                $newestFile = $filepath;
            }
        }
    }
    
    $filename = '';
    
    // Проверяем, можно ли дописывать в существующий файл
    if ($newestFile && $newestTime > 0) {
        $timeDiff = time() - $newestTime;
        
        if ($timeDiff <= 60) { // 60 секунд = 1 минута
            // Дописываем в существующий файл
            if (file_put_contents($newestFile, $input, FILE_APPEND | LOCK_EX) !== false) {
                $fileSize = filesize($newestFile);
                $filename = basename($newestFile);
                echo "Данные дописаны в файл: {$filename} ({$fileSize} байт)";
            } else {
                http_response_code(500);
                echo "Ошибка при дописывании в файл";
            }
            return;
        }
    }
    
    // Создаем новый файл (если нет подходящего файла или прошло больше минуты)
    $filename = 'audio_record_' . date('Y-m-d_H-i-s') . '.wav';
    $filepath = "uploads/{$filename}";
    
    if (file_put_contents($filepath, $input, LOCK_EX) !== false) {
        $fileSize = filesize($filepath);
        echo "Создан новый файл: {$filename} ({$fileSize} байт)";
    } else {
        http_response_code(500);
        echo "Ошибка при создании файла";
    }
    
} else {
    http_response_code(405);
    echo "Используйте POST запрос";
}
?>
