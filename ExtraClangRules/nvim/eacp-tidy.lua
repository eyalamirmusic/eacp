-- Surfaces the eacp clang-tidy checks as native nvim diagnostics for C++ /
-- Objective-C++ buffers in this repo. clangd cannot load clang-tidy plugins
-- (clangd/clangd#1458), so this runs the plugin-loaded clang-tidy wrapper as
-- an async linter instead. No plugin dependencies.
--
-- Wire it up from your nvim config:
--   dofile("/path/to/eacp/ExtraClangRules/nvim/eacp-tidy.lua").setup()
--
-- Lints the file on disk, so diagnostics refresh on open and on save. Only
-- the eacp-* checks run here; the rest of .clang-tidy comes from clangd.

local M = {}

local ns = vim.api.nvim_create_namespace("eacp-tidy")

local function lint(bufnr, wrapper, repo_root)
    local fname = vim.fs.normalize(vim.api.nvim_buf_get_name(bufnr))

    vim.system({
        wrapper,
        "-p",
        repo_root .. "/build",
        "--checks=-*,eacp-*",
        fname,
    }, { text = true }, function(out)
        local diags = {}
        for line in (out.stdout or ""):gmatch("[^\n]+") do
            local file, lnum, col, msg =
                line:match("^(.-):(%d+):(%d+): warning: (.+)$")
            if file and vim.fs.normalize(file) == fname then
                diags[#diags + 1] = {
                    lnum = tonumber(lnum) - 1,
                    col = tonumber(col) - 1,
                    severity = vim.diagnostic.severity.WARN,
                    message = msg,
                    source = "eacp-tidy",
                }
            end
        end
        vim.schedule(function()
            if vim.api.nvim_buf_is_valid(bufnr) then
                vim.diagnostic.set(ns, bufnr, diags)
            end
        end)
    end)
end

function M.setup(opts)
    opts = opts or {}
    local this_file = debug.getinfo(1, "S").source:sub(2)
    local tool_dir = opts.tool_dir
        or vim.fs.dirname(vim.fs.dirname(vim.fs.normalize(this_file)))
    local repo_root = vim.fs.dirname(tool_dir)
    local wrapper = tool_dir .. "/bin/eacp-clang-tidy"

    vim.api.nvim_create_autocmd({ "BufReadPost", "BufWritePost" }, {
        group = vim.api.nvim_create_augroup("EacpTidy", { clear = true }),
        pattern = { "*.cpp", "*.mm", "*.h" },
        callback = function(event)
            local name =
                vim.fs.normalize(vim.api.nvim_buf_get_name(event.buf))
            if name:sub(1, #repo_root + 1) == repo_root .. "/" then
                lint(event.buf, wrapper, repo_root)
            end
        end,
    })
end

return M
