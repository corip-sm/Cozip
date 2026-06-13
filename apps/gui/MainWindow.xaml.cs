using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Threading;
using Cozip.Gui.Models;

namespace Cozip.Gui
{
    public partial class MainWindow : Window
    {
        private readonly ObservableCollection<ArchiveEntryRow> _entries = new ObservableCollection<ArchiveEntryRow>();
        private readonly string _repoRoot;
        private readonly string _cliPath;
        private readonly Stopwatch _operationStopwatch = new Stopwatch();
        private readonly DispatcherTimer _elapsedTimer;
        private string _currentArchivePath = string.Empty;
        private bool _isBusy;
        private string _activeOperation = "Idle";

        public MainWindow()
        {
            InitializeComponent();

            EntriesListView.ItemsSource = _entries;

            _repoRoot = ResolveRepositoryRoot();
            _cliPath = ResolveCliPath(_repoRoot);

            BackendStatusTextBlock.Text = File.Exists(_cliPath)
                ? Path.GetFileName(_cliPath)
                : "cozip_cli.exe not found";

            _elapsedTimer = new DispatcherTimer();
            _elapsedTimer.Interval = TimeSpan.FromMilliseconds(120);
            _elapsedTimer.Tick += ElapsedTimer_OnTick;

            AppendLog("Cozip GUI shell ready.");
            AppendLog("Repository root: " + _repoRoot);
            AppendLog("CLI path: " + _cliPath);
            UpdateSummary();
        }

        private void OpenArchiveButton_OnClick(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Filter = "ZIP Archives (*.zip)|*.zip|All Files (*.*)|*.*",
                CheckFileExists = true,
                Multiselect = false,
                Title = "Open Archive"
            };

            if (dialog.ShowDialog(this) != true)
            {
                return;
            }

            _currentArchivePath = dialog.FileName;
            RefreshArchiveAsync();
        }

        private void RefreshButton_OnClick(object sender, RoutedEventArgs e)
        {
            RefreshArchiveAsync();
        }

        private void TestButton_OnClick(object sender, RoutedEventArgs e)
        {
            if (IsBlank(_currentArchivePath))
            {
                SetStatus("Open an archive first.");
                return;
            }

            RunSimpleArchiveCommandAsync("test", _currentArchivePath, "Integrity test finished.", null);
        }

        private void ExtractButton_OnClick(object sender, RoutedEventArgs e)
        {
            if (IsBlank(_currentArchivePath))
            {
                SetStatus("Open an archive first.");
                return;
            }

            using (var dialog = new System.Windows.Forms.FolderBrowserDialog())
            {
                dialog.Description = "Choose extraction folder";

                if (dialog.ShowDialog() != System.Windows.Forms.DialogResult.OK)
                {
                    return;
                }

                RunSimpleArchiveCommandAsync(
                    "extract",
                    _currentArchivePath,
                    "Extraction finished.",
                    dialog.SelectedPath);
            }
        }

        private void CreateZipButton_OnClick(object sender, RoutedEventArgs e)
        {
            var sourceDialog = new Microsoft.Win32.OpenFileDialog
            {
                Filter = "All Files (*.*)|*.*",
                CheckFileExists = true,
                Multiselect = true,
                Title = "Select files to archive"
            };

            if (sourceDialog.ShowDialog(this) != true || sourceDialog.FileNames.Length == 0)
            {
                return;
            }

            var saveDialog = new Microsoft.Win32.SaveFileDialog
            {
                Filter = "ZIP Archives (*.zip)|*.zip",
                Title = "Create ZIP Archive",
                AddExtension = true,
                DefaultExt = ".zip",
                FileName = "archive.zip"
            };

            if (saveDialog.ShowDialog(this) != true)
            {
                return;
            }

            var arguments = new List<string> { "create", "--balanced", Quote(saveDialog.FileName) };
            arguments.AddRange(sourceDialog.FileNames.Select(Quote).ToArray());

            RunCliAsync(
                "create",
                string.Join(" ", arguments.ToArray()),
                "Creating ZIP archive",
                delegate(CliResult result)
                {
                    if (result.ExitCode == 0)
                    {
                        _currentArchivePath = saveDialog.FileName;
                        SummaryActionTextBlock.Text = "Created ZIP archive";
                        RefreshArchiveAsync();
                    }
                    else
                    {
                        SetStatus("ZIP creation failed.");
                    }
                });
        }

        private void RefreshArchiveAsync()
        {
            if (IsBlank(_currentArchivePath))
            {
                SetStatus("Open an archive first.");
                return;
            }

            RunCliAsync(
                "list",
                "list " + Quote(_currentArchivePath),
                "Loading archive entries",
                delegate(CliResult result)
                {
                    if (result.ExitCode != 0)
                    {
                        SetStatus("Failed to load archive entries.");
                        return;
                    }

                    PopulateEntries(result.StandardOutput);
                    SummaryActionTextBlock.Text = "Loaded archive listing";
                    SetStatus("Archive loaded.");
                    UpdateSummary();
                });
        }

        private void RunSimpleArchiveCommandAsync(string command, string archivePath, string successMessage, string extraPath)
        {
            var builder = new StringBuilder();
            builder.Append(command).Append(' ').Append(Quote(archivePath));
            if (!IsBlank(extraPath))
            {
                builder.Append(' ').Append(Quote(extraPath));
            }

            RunCliAsync(
                command,
                builder.ToString(),
                ToPresentProgressText(command),
                delegate(CliResult result)
                {
                    if (result.ExitCode == 0)
                    {
                        SummaryActionTextBlock.Text = successMessage;
                        SetStatus(successMessage);
                    }
                    else
                    {
                        SetStatus(command + " failed.");
                    }
                });
        }

        private void RunCliAsync(string command, string arguments, string progressText, Action<CliResult> onCompleted)
        {
            if (!File.Exists(_cliPath))
            {
                var missingBackend = new CliResult(-1, string.Empty, "Backend not found: " + _cliPath, 0);
                AppendCommandResult(command, missingBackend);
                if (onCompleted != null)
                {
                    onCompleted(missingBackend);
                }
                return;
            }

            if (_isBusy)
            {
                SetStatus("Another operation is already running.");
                return;
            }

            var worker = new BackgroundWorker();
            worker.DoWork += delegate(object sender, DoWorkEventArgs e)
            {
                e.Result = RunCliCore(arguments);
            };
            worker.RunWorkerCompleted += delegate(object sender, RunWorkerCompletedEventArgs e)
            {
                var result = e.Error != null
                    ? new CliResult(-1, string.Empty, e.Error.Message, _operationStopwatch.ElapsedMilliseconds)
                    : (CliResult)e.Result;

                StopBusyState();
                AppendCommandResult(command, result);

                if (onCompleted != null)
                {
                    onCompleted(result);
                }
            };

            AppendLog("> cozip_cli " + command + " started");
            StartBusyState(progressText);
            worker.RunWorkerAsync();
        }

        private CliResult RunCliCore(string arguments)
        {
            var startInfo = new ProcessStartInfo
            {
                FileName = _cliPath,
                Arguments = arguments,
                WorkingDirectory = Path.GetDirectoryName(_cliPath) ?? _repoRoot,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true
            };

            var stopwatch = Stopwatch.StartNew();
            using (var process = new Process { StartInfo = startInfo })
            {
                process.Start();
                var stdout = process.StandardOutput.ReadToEnd();
                var stderr = process.StandardError.ReadToEnd();
                process.WaitForExit();
                stopwatch.Stop();
                return new CliResult(process.ExitCode, stdout, stderr, stopwatch.ElapsedMilliseconds);
            }
        }

        private void PopulateEntries(string output)
        {
            _entries.Clear();
            var lines = output
                .Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries)
                .Where(line => !line.StartsWith("zip entries=", StringComparison.OrdinalIgnoreCase))
                .ToList();

            var regex = new Regex(
                @"^(?<kind>[df])\s+(?<size>\d+)\s+(?<packed>\d+)\s+method=(?<method>[^(]+)\((?<methodId>\d+)\)\s+crc=0x(?<crc>[0-9a-fA-F]{8})\s+(?<name>.+)$",
                RegexOptions.Compiled);

            foreach (var line in lines)
            {
                var match = regex.Match(line);
                if (!match.Success)
                {
                    continue;
                }

                var size = ParseUInt64(match.Groups["size"].Value);
                var packed = ParseUInt64(match.Groups["packed"].Value);
                _entries.Add(new ArchiveEntryRow
                {
                    Kind = match.Groups["kind"].Value == "d" ? "Dir" : "File",
                    Name = match.Groups["name"].Value,
                    Method = match.Groups["method"].Value.Trim(),
                    Size = size,
                    PackedSize = packed,
                    SizeDisplay = FormatBytes(size),
                    PackedDisplay = FormatBytes(packed),
                    Crc32 = "0x" + match.Groups["crc"].Value.ToUpperInvariant()
                });
            }

            EntryCountTextBlock.Text = _entries.Count.ToString(CultureInfo.InvariantCulture) + " items";
            CurrentArchiveTextBlock.Text = _currentArchivePath;
        }

        private void AppendCommandResult(string command, CliResult result)
        {
            AppendLog("> cozip_cli " + command);
            if (!IsBlank(result.StandardOutput))
            {
                AppendLog(result.StandardOutput.TrimEnd());
            }

            if (!IsBlank(result.StandardError))
            {
                AppendLog(result.StandardError.TrimEnd());
            }

            AppendLog("exit=" + result.ExitCode.ToString(CultureInfo.InvariantCulture));
            AppendLog("elapsed=" + result.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms");
        }

        private void AppendLog(string line)
        {
            LogTextBox.AppendText("[" + DateTime.Now.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture) + "] " + line + Environment.NewLine);
            LogTextBox.ScrollToEnd();
        }

        private void UpdateSummary()
        {
            SummaryPathTextBlock.Text = IsBlank(_currentArchivePath) ? "-" : _currentArchivePath;
            SummaryEntriesTextBlock.Text = _entries.Count.ToString(CultureInfo.InvariantCulture);
            SummarySizeTextBlock.Text = FormatBytes((ulong)_entries.Sum(item => (long)item.Size));
        }

        private void SetBusy(bool isBusy)
        {
            _isBusy = isBusy;
            CreateZipButton.IsEnabled = !isBusy;
            OpenArchiveButton.IsEnabled = !isBusy;
            ExtractButton.IsEnabled = !isBusy;
            TestButton.IsEnabled = !isBusy;
            RefreshButton.IsEnabled = !isBusy;
        }

        private void SetStatus(string text)
        {
            StatusTextBlock.Text = text;
        }

        private static bool IsBlank(string value)
        {
            return string.IsNullOrEmpty(value) || value.Trim().Length == 0;
        }

        private static ulong ParseUInt64(string value)
        {
            ulong.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed);
            return parsed;
        }

        private static string FormatBytes(ulong bytes)
        {
            string[] suffixes = { "B", "KB", "MB", "GB", "TB" };
            double size = bytes;
            var suffixIndex = 0;
            while (size >= 1024 && suffixIndex < suffixes.Length - 1)
            {
                size /= 1024;
                suffixIndex++;
            }

            return size.ToString(size >= 100 ? "0" : "0.0", CultureInfo.InvariantCulture) + " " + suffixes[suffixIndex];
        }

        private static string Quote(string value)
        {
            return "\"" + value.Replace("\"", "\\\"") + "\"";
        }

        private static string ResolveRepositoryRoot()
        {
            var baseDirectory = AppDomain.CurrentDomain.BaseDirectory;
            var candidate = Path.GetFullPath(
                Path.Combine(
                    Path.Combine(
                        Path.Combine(Path.Combine(baseDirectory, ".."), ".."),
                        ".."),
                    ".."));
            if (Directory.Exists(candidate))
            {
                return candidate;
            }

            return baseDirectory;
        }

        private static string ResolveCliPath(string repositoryRoot)
        {
            var releasePath = Path.Combine(
                Path.Combine(
                    Path.Combine(repositoryRoot, "build-release"),
                    "apps"),
                Path.Combine("cli", "cozip_cli.exe"));
            if (File.Exists(releasePath))
            {
                return releasePath;
            }

            return Path.Combine(
                Path.Combine(
                    Path.Combine(Path.Combine(repositoryRoot, "build"), "apps"),
                    "cli"),
                Path.Combine("Debug", "cozip_cli.exe"));
        }

        private void StartBusyState(string progressText)
        {
            _activeOperation = progressText;
            SetBusy(true);
            SetStatus(progressText + "...");
            OperationStateTextBlock.Text = progressText;
            OperationProgressBar.Visibility = Visibility.Visible;
            OperationProgressBar.IsIndeterminate = true;
            _operationStopwatch.Reset();
            _operationStopwatch.Start();
            ElapsedTextBlock.Text = "0 ms";
            _elapsedTimer.Start();
        }

        private void StopBusyState()
        {
            _elapsedTimer.Stop();
            _operationStopwatch.Stop();
            OperationProgressBar.IsIndeterminate = false;
            OperationProgressBar.Visibility = Visibility.Collapsed;
            OperationStateTextBlock.Text = "Idle";
            ElapsedTextBlock.Text = _operationStopwatch.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms";
            SetBusy(false);
            if (StatusTextBlock.Text == "Working...")
            {
                StatusTextBlock.Text = "Ready";
            }
            _activeOperation = "Idle";
        }

        private void ElapsedTimer_OnTick(object sender, EventArgs e)
        {
            var elapsed = _operationStopwatch.ElapsedMilliseconds;
            ElapsedTextBlock.Text = elapsed.ToString(CultureInfo.InvariantCulture) + " ms";
            StatusTextBlock.Text = _activeOperation + "... " + elapsed.ToString(CultureInfo.InvariantCulture) + " ms";
        }

        private static string ToPresentProgressText(string command)
        {
            if (string.Equals(command, "create", StringComparison.OrdinalIgnoreCase))
            {
                return "Creating archive";
            }

            if (string.Equals(command, "extract", StringComparison.OrdinalIgnoreCase))
            {
                return "Extracting archive";
            }

            if (string.Equals(command, "test", StringComparison.OrdinalIgnoreCase))
            {
                return "Testing archive";
            }

            if (string.Equals(command, "list", StringComparison.OrdinalIgnoreCase))
            {
                return "Loading archive entries";
            }

            return "Working";
        }

        private sealed class CliResult
        {
            public CliResult(int exitCode, string standardOutput, string standardError, long elapsedMilliseconds)
            {
                ExitCode = exitCode;
                StandardOutput = standardOutput ?? string.Empty;
                StandardError = standardError ?? string.Empty;
                ElapsedMilliseconds = elapsedMilliseconds;
            }

            public int ExitCode { get; }
            public string StandardOutput { get; }
            public string StandardError { get; }
            public long ElapsedMilliseconds { get; }
        }
    }
}
