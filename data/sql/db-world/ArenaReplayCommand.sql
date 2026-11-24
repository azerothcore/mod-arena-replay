DELETE FROM `command` WHERE `name` = 'replay';
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('replay', 0, 'Syntax: .replay <id>\nWatch the replay with the given id.');
