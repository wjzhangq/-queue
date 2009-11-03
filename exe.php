#!/opt/local/bin/php -f
<?php
sleep(5);
file_put_contents('log.txt', var_export($argv, true) . "\n", FILE_APPEND);
print_r($argv);