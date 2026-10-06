;;; ddskk_driver.el --- Isolated behavioral comparison -*- coding: utf-8; lexical-binding: t; -*-
;; Invoked only by scripts/compare-ddskk.ps1 with -Q --batch and a fresh HOME.
;; No user init, installed SKK package, network dictionary, or personal data.
(setq user-emacs-directory (file-name-as-directory (getenv "NSKKZIME_COMPARE_ROOT"))
      default-directory user-emacs-directory
      skk-user-directory user-emacs-directory
      skk-init-file (expand-file-name "unused-init.el" user-emacs-directory)
      skk-share-private-jisyo nil)
(require 'loaddefs-gen)
(let ((generated (getenv "NSKKZIME_COMPARE_AUTOLOADS")))
  (add-to-list 'load-path (file-name-directory generated))
  (unless (file-exists-p generated)
    (loaddefs-generate (getenv "NSKKZIME_DDSKK_SOURCE") generated))
  (load generated nil t))
(require 'skk)
(require 'json)
(setq skk-jisyo-code 'utf-8
      skk-large-jisyo (cons (getenv "NSKKZIME_COMPARE_DICT") 'utf-8)
      skk-search-prog-list '((skk-search-jisyo-file skk-jisyo 0 t)
                             (skk-search-jisyo-file skk-large-jisyo 0))
      skk-save-jisyo-instantly nil
      skk-keep-record nil
      skk-server-host nil
      skk-show-mode-show nil
      skk-show-annotation nil
      skk-auto-insert-paren nil)

(defun nskkzime-comparison-keys (text)
  "Translate the skk_trace notation TEXT into Emacs keyboard events."
  (let ((position 0) events)
    (while (< position (length text))
      (if (= (aref text position) ?<)
          (let* ((end (string-match ">" text position))
                 (name (and end (substring text (1+ position) end)))
                 (entry (assoc name '(("Space" . 32) ("Enter" . 13) ("C-j" . 10)
                                      ("C-g" . 7) ("BS" . 127)
                                      ("PageDown" . next) ("PageUp" . prior)))))
            (unless entry (error "Unsupported comparison token %s" name))
            (push (cdr entry) events)
            (setq position (1+ end)))
        (push (aref text position) events)
        (setq position (1+ position))))
    (vconcat (nreverse events))))

;; Begin with the same empty private dictionary, avoiding creation notices and
;; their fixed waits without replacing any DDSKK input command.
(with-temp-file (skk-jisyo))
(switch-to-buffer (get-buffer-create "nskkzime-comparison"))
(skk-mode 1)
(unwind-protect
    (progn
      (execute-kbd-macro (nskkzime-comparison-keys (getenv "NSKKZIME_COMPARE_KEYS")))
      (let ((coding-system-for-write 'utf-8-unix)
            (result `((emacs . ,emacs-version)
                      (text . ,(buffer-substring-no-properties (point-min) (point-max)))
                      (henkan . ,(symbol-name skk-henkan-mode)))))
        (with-temp-file (getenv "NSKKZIME_COMPARE_RESULT")
          (insert (json-encode result) "\n"))))
  (remove-hook 'kill-emacs-hook 'skk-save-jisyo))
