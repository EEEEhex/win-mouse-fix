using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Diagnostics;
using WinMouseFix.Settings.Resources;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Windowing;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Storage.Pickers;
using Windows.System;
using WinMouseFix.Configuration;
using WinMouseFix.Settings.Services;
using WinMouseFix.Settings.ViewModels;

namespace WinMouseFix.Settings;
public sealed partial class MainWindow : Window
{
    public SettingsModel Model { get; private set; }
    private readonly SettingsEndpoint _endpoint;
    private readonly IHostSession _session;
    private Process? _hostProcess;
    private bool _hostLost;
    private ToggleSwitch? _automaticUpdates;
    private ToggleSwitch? _startup, _startupNotice;
    private Expander? _moreSettings, _advancedSettings;
    private readonly Dictionary<uint,ComboBox> _bindingActions = [];
    private readonly List<Control> _bindingControls = [];
    private string _bindingStructure = "";
    private const string RepositoryUrl = "https://github.com/EEEEhex/win-mouse-fix";
    private readonly List<Action<ProductConfiguration>> _refresh = [];
    private readonly Dictionary<Control,Func<bool>> _hostControls = [];
    private bool _lastConnected;
    private string? _uiError;
    private bool _updating, _allowClose, _closing;
    private string _page = "general";
    private StackPanel? _bindings;
    private ToggleSwitch? _enabled;
    private TextBlock? _runtime, _capability;
    private ComboBox? _identifyButton;
    private CheckBox[]? _identifyModifiers;
    private ContentDialog? _bindingDialog;
    private Controls.BindingEditor? _bindingEditor;
    private ulong _lastIdentifiedSerial;
    private UiPreferences _preferences;
    public MainWindow(SettingsEndpoint endpoint, IHostSession session, bool smoke = false)
    {
        _endpoint = endpoint; _session = session; _preferences = UiPreferences.Load(endpoint.PreferencesDirectory); UiText.Language = _preferences.Language; InitializeComponent(); LocalizeNavigation();
        ExtendsContentIntoTitleBar = true; SetTitleBar(TitleBar);
        Model = new(session, action => DispatcherQueue.TryEnqueue(() => action()));
        Root.SizeChanged += (_,_) => PageContent.Width = Math.Max(0,Math.Min(700,Root.ActualWidth-64));
        WireModel();
        Root.RequestedTheme = _preferences.Theme;
        AppWindow.Resize(new(960,800)); _preferences.Apply(AppWindow);
        var icon = Path.Combine(AppContext.BaseDirectory,"Assets/wmf.ico"); if (File.Exists(icon)) AppWindow.SetIcon(icon);
        AppWindow.Closing += async (_,args) => {
            if (_allowClose) return; args.Cancel = true; if (_closing) return; _closing = true;
            try { if (!_hostLost) { if (Model.Status.Recording) await Model.RecordAsync(false); await Model.FlushAsync(); } await Model.DisposeAsync(); }
            finally { _hostProcess?.Dispose(); _preferences.Page = _page; _preferences.Theme = Root.RequestedTheme; _preferences.Save(endpoint.PreferencesDirectory,AppWindow); _allowClose = true; Close(); }
        };
        Navigation.SelectedItem = Navigation.MenuItems.OfType<NavigationViewItem>().FirstOrDefault(i => (string)i.Tag == _preferences.Page) ?? Navigation.MenuItems[0];
    }
    public async Task ConnectAsync() {
        await Model.ConnectAsync(WinRT.Interop.WindowNative.GetWindowHandle(this));
        if (Model.Connected && !_preferences.UpdatePreferencesMigrated) {
            bool saved = true;
            if (_preferences.AutomaticUpdates && !Model.Committed.AutomaticUpdates) { Model.Edit(c => c.AutomaticUpdates = true); saved = await Model.FlushAsync(); }
            if (saved) { _preferences.UpdatePreferencesMigrated = true; _preferences.Save(_endpoint.PreferencesDirectory,AppWindow); }
        }
        if (!_endpoint.Simulation && Model.Connected && _session is HostClient client) {
            _hostProcess = Process.GetProcessById(checked((int)client.HostPid));
            _hostProcess.Exited += (_,_) => DispatcherQueue.TryEnqueue(() => { _hostLost = true; Close(); });
            _hostProcess.EnableRaisingEvents = true;
            if (_hostProcess.HasExited) throw new IOException(UiText.Get("后台已退出。"));
        }
    }
    private void LocalizeNavigation() {
        var labels = new[]{UiText.Get("通用"),UiText.Get("按键"),UiText.Get("滚动"),UiText.Get("关于")};
        for (int i = 0; i < labels.Length; ++i) {
            var item = (NavigationViewItem)Navigation.MenuItems[i]; ((StackPanel)item.Content).Children.OfType<TextBlock>().Single().Text = labels[i]; AutomationProperties.SetName(item,labels[i]);
        }
        if (ConnectionBar.ActionButton is Button reload) reload.Content = UiText.Get("重新读取");
    }
    private void WireModel() { Model.PropertyChanged += (_,_) => RefreshState(); Model.ConfigurationChanged += RefreshControls; }
    private static TextBlock Text(string text, bool heading = false) => new() { Text = text, TextWrapping = TextWrapping.Wrap, FontSize = heading ? 26 : 14,
        FontWeight = heading ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal };
    private static StackPanel Stack(params UIElement[] items) { var panel = new StackPanel { Spacing = 12 }; foreach(var item in items) panel.Children.Add(item); return panel; }
    private static Border Card(UIElement content) => new() { Child = content, Style = (Style)Application.Current.Resources["CardBorderStyle"] };
    private Button Button(string title, Func<Task> action) { var button = new Button { Content = title }; AutomationProperties.SetName(button,title);
        button.Click += async (_,_) => { _uiError = null; try { await action(); } catch(Exception e) { _uiError = e.Message; RefreshState(); } }; return button; }
    private T RequiresHost<T>(T control,Func<bool>? available = null) where T:Control { _hostControls.Add(control,available ?? (() => true)); return control; }
    private void Navigation_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args) { if (Model is null || args.SelectedItem is not NavigationViewItem item) return; _page = (string)item.Tag; BuildPage(); }
    public void Navigate(string tag) { Navigation.SelectedItem = Navigation.MenuItems.OfType<NavigationViewItem>().First(i => (string)i.Tag == tag); }
    private void BuildPage() {
        _refresh.Clear(); _hostControls.Clear(); PageContent.Children.Clear(); _bindings = null; _bindingActions.Clear(); _bindingControls.Clear(); _bindingStructure = ""; _enabled = null; _automaticUpdates = null; _startup = null; _startupNotice = null; _moreSettings = null; _advancedSettings = null; _runtime = null; _capability = null;
        switch (_page) { case "general": General(); break; case "buttons": Buttons(); break; case "scrolling": Scrolling(); break; default: About(); break; }
        RefreshControls(); RefreshState();
        PageScroll.ChangeView(null,0,null,true);
    }
    private static Border Separator() => new() { Height = 1, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(50,128,128,128)), Margin = new(0,6,0,6) };
    private static TextBlock Secondary(string text) { var block = Text(text); block.Opacity = 0.65; return block; }
    private static Grid SettingRow(string label,Control control,double fontSize = 16) {
        var row = new Grid { ColumnSpacing = 24 };
        row.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); row.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var title = Text(label); title.FontSize = fontSize; title.VerticalAlignment = VerticalAlignment.Center;
        control.MinWidth = 0; control.HorizontalAlignment = HorizontalAlignment.Right; control.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetName(control,label); row.Children.Add(title); Grid.SetColumn(control,1); row.Children.Add(control); return row;
    }
    private void General() {
        _enabled = new() { OnContent = "", OffContent = "", MinWidth = 0, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetName(_enabled,UiText.Get("启动 Win Mouse Fix"));
        _enabled.Toggled += async (_,_) => { if (!_updating && Model.Connected) await Model.SetEnabledAsync(_enabled.IsOn); };
        var enabledRow = new Grid { ColumnSpacing = 24 }; enabledRow.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); enabledRow.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var heading = Text(UiText.Get("启动 Win Mouse Fix"),true); heading.FontSize = 22; heading.VerticalAlignment = VerticalAlignment.Center;
        enabledRow.Children.Add(heading); Grid.SetColumn(_enabled,1); enabledRow.Children.Add(_enabled);
        _runtime = Secondary("");
        PageContent.Children.Add(Stack(enabledRow,Secondary(UiText.Get("关闭设置窗口后，Win Mouse Fix 将继续在托盘运行。")),_runtime));
        PageContent.Children.Add(Separator());
        var startup = _startup = RequiresHost(new ToggleSwitch { OnContent = "", OffContent = "" });
        startup.Toggled += async (_,_) => { if (!_updating) await Model.SetStartupAsync(startup.IsOn); }; _refresh.Add(c => startup.IsOn = c.Startup);
        var startupNotice = _startupNotice = RequiresHost(new ToggleSwitch { OnContent = "", OffContent = "" });
        startupNotice.Toggled += async (_,_) => { if (!_updating) { Model.Edit(c => c.StartupNotification = startupNotice.IsOn); await Model.FlushAsync(); } }; _refresh.Add(c => startupNotice.IsOn = c.StartupNotification);
        PageContent.Children.Add(Stack(SettingRow(UiText.Get("登录 Windows 时启动"),startup),SettingRow(UiText.Get("启动提示"),startupNotice),Secondary(UiText.Get("程序启动时显示后台运行通知，下次启动生效。"))));
        PageContent.Children.Add(Separator());
        _automaticUpdates = RequiresHost(new ToggleSwitch { OnContent = "", OffContent = "" });
        AutomationProperties.SetName(_automaticUpdates,UiText.Get("自动更新"));
        _automaticUpdates.Toggled += async (_,_) => { if (!_updating) { Model.Edit(c => c.AutomaticUpdates = _automaticUpdates.IsOn); await Model.FlushAsync(); } }; _refresh.Add(c => _automaticUpdates.IsOn = c.AutomaticUpdates);
        var checkUpdates = RequiresHost(Button(UiText.Get("检查更新"),CheckForUpdatesAsync)); checkUpdates.HorizontalAlignment = HorizontalAlignment.Right; checkUpdates.VerticalAlignment = VerticalAlignment.Center;
        var updateDetails = new Grid { ColumnSpacing = 24 }; updateDetails.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); updateDetails.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var updateNote = Secondary(UiText.Get("启动时及按设定间隔检查，有新版本时提醒。")); updateNote.VerticalAlignment = VerticalAlignment.Center; updateDetails.Children.Add(updateNote); Grid.SetColumn(checkUpdates,1); updateDetails.Children.Add(checkUpdates);
        var interval = RequiresHost(new NumberBox { Minimum = 1, Maximum = 168, SmallChange = 1, LargeChange = 6, Width = 130, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Inline });
        interval.ValueChanged += (_,_) => { if (!_updating && double.IsFinite(interval.Value)) { var hours = (uint)Math.Clamp(Math.Round(interval.Value,MidpointRounding.AwayFromZero),1,168); if (interval.Value != hours) interval.Value = hours; else Model.Edit(c => c.UpdateIntervalHours = hours); } };
        _refresh.Add(c => interval.Value = c.UpdateIntervalHours);
        PageContent.Children.Add(Stack(SettingRow(UiText.Get("自动更新"),_automaticUpdates),updateDetails,SettingRow(UiText.Get("检查间隔（小时）"),interval)));
        PageContent.Children.Add(Separator());
        var actions = new Grid { ColumnSpacing = 10 }; for(int i = 0; i < 3; ++i) actions.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) });
        var actionButtons = new[]{RequiresHost(Button(UiText.Get("导入设置"),ImportAsync)),RequiresHost(Button(UiText.Get("导出设置"),ExportAsync)),RequiresHost(Button(UiText.Get("恢复默认"),ResetAsync))};
        for(int i = 0; i < actionButtons.Length; ++i) { actionButtons[i].HorizontalAlignment = HorizontalAlignment.Stretch; Grid.SetColumn(actionButtons[i],i); actions.Children.Add(actionButtons[i]); }
        var theme = new ComboBox { Header = UiText.Get("外观"), HorizontalAlignment = HorizontalAlignment.Stretch, ItemsSource = new[]{UiText.Get("跟随系统"),UiText.Get("浅色"),UiText.Get("深色")}, SelectedIndex = (int)Root.RequestedTheme };
        theme.SelectionChanged += (_,_) => { Root.RequestedTheme = _preferences.Theme = (ElementTheme)theme.SelectedIndex; _preferences.Save(_endpoint.PreferencesDirectory,AppWindow); };
        var language = new ComboBox { Header = UiText.Get("语言"), HorizontalAlignment = HorizontalAlignment.Stretch, ItemsSource = new[]{"简体中文","English"}, SelectedIndex = UiText.Language == "zh-Hans" ? 0 : 1 };
        language.SelectionChanged += async (_,_) => { _preferences.Language = UiText.Language = language.SelectedIndex == 0 ? "zh-Hans" : "en"; _preferences.Save(_endpoint.PreferencesDirectory,AppWindow); LocalizeNavigation(); BuildPage(); if (Model.Connected) await Model.ReloadAsync(); };
        var preferences = new Grid { ColumnSpacing = 20 }; preferences.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); preferences.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) });
        preferences.Children.Add(theme); Grid.SetColumn(language,1); preferences.Children.Add(language);
        var moreContent = Stack(preferences,Separator(),actions); moreContent.Margin = new(0,12,0,0);
        _moreSettings = new Expander { Header = UiText.Get("更多设置"), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch, Content = moreContent }; PageContent.Children.Add(_moreSettings);
    }
    private void Buttons() {
        _bindings = new() { Spacing = 0 };
        PageContent.Children.Add(new Border { Style = (Style)Application.Current.Resources["BindingTableBorderStyle"], Child = _bindings });
        var footer = new Grid { ColumnSpacing = 16 }; footer.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); footer.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var add = RequiresHost(Button(UiText.Get("添加操作"),() => EditBindingAsync(null))); add.HorizontalAlignment = HorizontalAlignment.Left; footer.Children.Add(add);
        var reset = RequiresHost(Button(UiText.Get("恢复默认按键"),ResetBindingsAsync)); Grid.SetColumn(reset,1); footer.Children.Add(reset); PageContent.Children.Add(footer);
    }
    private static string[] BindingActionOptions(string trigger) => trigger == "drag" ? ["content-drag","shell-gestures"] : trigger == "wheel" ? ["native-zoom"] : ActionCatalog.Discrete;
    private static string BindingActionName(string action) => action switch { "content-drag" => UiText.Get("滚动内容"), "shell-gestures" => UiText.Get("任务视图与桌面"), "native-zoom" => UiText.Get("放大或缩小"), _ => ActionName(action) };
    private static Border TableCell(UIElement content,bool bottom = true) => new() { Style = (Style)Application.Current.Resources["BindingTableCellStyle"], Child = content, BorderThickness = new(0,0,0,bottom ? 1 : 0) };
    private void RefreshBindings() {
        if (_bindings is null) return;
        var ordered = Model.Draft.Bindings.OrderBy(b => b.Button == "back" ? 0 : b.Button == "forward" ? 1 : 2).ThenBy(b => b.Modifiers).ThenBy(b => b.Trigger switch { "click" => 0, "double-click" => 1, "hold" => 2, "wheel" or "wheel-up" => 3, "wheel-down" => 4, _ => 5 }).ThenBy(b => b.Id).ToArray();
        var structure = string.Join(";",ordered.Select(b => $"{b.Id}/{b.Button}/{b.Modifiers}/{b.Trigger}"));
        bool wasUpdating = _updating; _updating = true;
        try {
        if (_bindingStructure != structure || _bindings.Children.Count == 0) {
            _bindingStructure = structure; _bindings.Children.Clear(); _bindingActions.Clear(); _bindingControls.Clear();
            if (ordered.Length == 0) { var empty = Secondary(UiText.Get("还没有按键操作，点击下方“添加操作”开始设置。")); empty.Margin = new(20); _bindings.Children.Add(empty); }
            foreach(var group in ordered.GroupBy(b => b.Button)) {
                var header = Text(group.Key == "back" ? UiText.Get("按键 4 · 后退侧键") : group.Key == "forward" ? UiText.Get("按键 5 · 前进侧键") : ButtonName(group.Key)); header.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
                _bindings.Children.Add(new Border { Style = (Style)Application.Current.Resources["BindingGroupHeaderStyle"], Child = header });
                foreach(var binding in group) {
                    var row = new Grid { MinHeight = 56 }; row.ColumnDefinitions.Add(new() { Width = new(48) }); row.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); row.ColumnDefinitions.Add(new() { Width = new(1.15,GridUnitType.Star) }); row.ColumnDefinitions.Add(new() { Width = new(44) });
                    bool bottom = binding.Id != ordered[^1].Id;
                    var remove = Button(UiText.Get("删除操作"),() => DeleteBindingAsync(binding.Id)); remove.Content = new FontIcon { Glyph = "\uE738", FontSize = 12 }; remove.Width = 28; remove.Height = 28; remove.Padding = new(0); remove.VerticalAlignment = VerticalAlignment.Center; remove.HorizontalAlignment = HorizontalAlignment.Center; _bindingControls.Add(remove);
                    AutomationProperties.SetName(remove,UiText.Format("删除 {0} 的 {1}",ButtonName(binding.Button),TriggerName(binding.Trigger))); ToolTipService.SetToolTip(remove,UiText.Get("删除操作")); row.Children.Add(TableCell(remove,bottom));
                    var trigger = Text(Modifiers(binding.Modifiers)+TriggerName(binding.Trigger)); trigger.VerticalAlignment = VerticalAlignment.Center; trigger.Margin = new(4,12,12,12); var triggerCell = TableCell(trigger,bottom); Grid.SetColumn(triggerCell,1); row.Children.Add(triggerCell);
                    var selector = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Center, MinWidth = 0, Margin = new(12,10,0,10) };
                    AutomationProperties.SetName(selector,UiText.Format("{0} · {1}的动作",ButtonName(binding.Button),TriggerName(binding.Trigger)));
                    foreach(var action in BindingActionOptions(binding.Trigger)) selector.Items.Add(new ComboBoxItem { Tag = action, Content = new TextBlock { Text = BindingActionName(action), TextTrimming = TextTrimming.CharacterEllipsis } });
                    selector.SelectionChanged += async (_,_) => { if (_updating || selector.SelectedItem is not ComboBoxItem selected) return; await ChangeBindingActionAsync(binding.Id,(string)selected.Tag); };
                    _bindingActions.Add(binding.Id,selector); _bindingControls.Add(selector); var actionCell = TableCell(selector,bottom); actionCell.BorderThickness = new(1,0,0,bottom ? 1 : 0); Grid.SetColumn(actionCell,2); row.Children.Add(actionCell);
                    var edit = Button(UiText.Get("编辑绑定"),() => EditBindingAsync(Model.Draft.Bindings.FirstOrDefault(b => b.Id == binding.Id))); edit.Content = new FontIcon { Glyph = "\uE70F", FontSize = 14 }; edit.Padding = new(0); edit.Width = 28; edit.Height = 28; edit.HorizontalAlignment = HorizontalAlignment.Center; edit.VerticalAlignment = VerticalAlignment.Center;
                    AutomationProperties.SetName(edit,UiText.Format("编辑 {0} 的 {1}",ButtonName(binding.Button),TriggerName(binding.Trigger))); ToolTipService.SetToolTip(edit,UiText.Get("编辑触发方式与快捷键")); _bindingControls.Add(edit); var editCell = TableCell(edit,bottom); Grid.SetColumn(editCell,3); row.Children.Add(editCell); _bindings.Children.Add(row);
                }
            }
        }
        foreach(var binding in ordered) {
            var selector = _bindingActions[binding.Id]; selector.SelectedItem = selector.Items.OfType<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == binding.Action);
            ToolTipService.SetToolTip(selector,binding.Action == "shortcut" ? string.Join(" + ",binding.Keys.Select(KeyName)) : ActionName(binding.Action));
        }
        RefreshBindingAvailability();
        } finally { _updating = wasUpdating; }
    }
    private void RefreshBindingAvailability() {
        foreach(var control in _bindingControls) control.IsEnabled = Model.Connected && !_closing && !Model.Saving;
        foreach(var selector in _bindingActions.Values) foreach(var item in selector.Items.OfType<ComboBoxItem>()) item.IsEnabled = (string)item.Tag != "native-zoom" || ZoomAvailable();
    }
    private async Task ChangeBindingActionAsync(uint id,string action) {
        var binding = Model.Draft.Bindings.FirstOrDefault(b => b.Id == id);
        if (!Model.Connected || binding is null || binding.Action == action) { RefreshBindings(); return; }
        try {
            if (!BindingActionOptions(binding.Trigger).Contains(action) || (action == "native-zoom" && !ZoomAvailable())) return;
            if (action == "shortcut") {
                var draft = JsonSerializer.Deserialize<ButtonBinding>(JsonSerializer.Serialize(binding,ConfigurationJson.Options),ConfigurationJson.Options)!; draft.Action = action; draft.Keys.Clear(); await EditBindingAsync(draft);
            } else {
                Model.Edit(c => { var current = c.Bindings.First(b => b.Id == id); current.Action = action; current.Keys.Clear(); }); await Model.FlushAsync();
            }
        } catch(Exception error) { _uiError = error.Message; RefreshState(); }
        finally { RefreshBindings(); }
    }
    private async Task DeleteBindingAsync(uint id) {
        var binding = Model.Draft.Bindings.FirstOrDefault(b => b.Id == id); if (binding is null) return;
        if (!await ConfirmAsync(UiText.Get("删除操作"),UiText.Format("将移除 {0} 的“{1}”操作。",ButtonName(binding.Button),TriggerName(binding.Trigger)))) return;
        Model.Edit(c => c.Bindings.RemoveAll(b => b.Id == id)); await Model.FlushAsync(); RefreshBindings();
    }
    private async Task ResetBindingsAsync() {
        if (!await ConfirmAsync(UiText.Get("恢复默认按键"),UiText.Get("将恢复默认按键绑定，其他设置保持不变。"))) return;
        Model.Edit(c => c.Bindings = new ProductConfiguration().Bindings); await Model.FlushAsync(); RefreshBindings();
    }
    private NumberBox Number(string title,Func<ProductConfiguration,double> read,Action<ProductConfiguration,double> write,double min = 0.000001,double max = 100,Func<bool>? available = null) {
        var box = RequiresHost(new NumberBox { Header = title, Minimum = min, Maximum = max, SmallChange = 0.01, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact },available);
        AutomationProperties.SetName(box,title); _refresh.Add(c => { if (box.FocusState == FocusState.Unfocused) box.Value = read(c); });
        box.ValueChanged += (_,e) => { if (!_updating) Model.Edit(c => write(c,e.NewValue)); }; return box;
    }
    private CheckBox Reverse(string title,Func<ProductConfiguration,double> read,Action<ProductConfiguration,double> write,Func<bool>? available = null) {
        var control = RequiresHost(new CheckBox { Content = title },available); _refresh.Add(c => control.IsChecked = read(c) < 0);
        control.Click += (_,_) => { if (!_updating) Model.Edit(c => write(c,Math.Abs(read(c))*(control.IsChecked == true ? -1 : 1))); }; return control;
    }
    private bool WheelAvailable() => Model.Draft.Wheel.Mode != "off" && Model.Status.Capabilities.Any(c => c.Name == "wheel" && c.State == "supported");
    private bool ZoomAvailable() => Model.Status.Capabilities.Any(c => c.Name == "zoom" && c.State == "supported");
    private static StackPanel AdvancedPair(string title,NumberBox left,NumberBox right) {
        var heading = Text(title); heading.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        var columns = new Grid { ColumnSpacing = 24 };
        columns.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); columns.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) });
        foreach(var box in new[]{left,right}) { box.Header = Text((string)box.Header); box.MinWidth = 0; box.HorizontalAlignment = HorizontalAlignment.Stretch; }
        columns.Children.Add(left); Grid.SetColumn(right,1); columns.Children.Add(right);
        return Stack(heading,columns);
    }
    private void Scrolling() {
        PageContent.Children.Add(Text(UiText.Get("滚动与手势"),true));
        var zoomBindings = Text("");
        _refresh.Add(c => zoomBindings.Text = string.Join("、",c.Bindings.Where(b => b.Action == "native-zoom").Select(b => Modifiers(b.Modifiers)+ButtonName(b.Button))));
        PageContent.Children.Add(Card(Stack(Text(UiText.Get("缩放")),Text(UiText.Get("按住侧键并滚动滚轮进行缩放；到达边界后停住，反向可继续。")),zoomBindings,
            RequiresHost(Button(UiText.Get("设置后退侧键＋滚轮缩放"),async () => {
                bool conflict = Model.Draft.Bindings.Any(b => b.Button == "back" && b.Modifiers == 0 && (b.Trigger is "wheel" or "wheel-up" or "wheel-down") && b.Action != "native-zoom");
                if (conflict && !await ConfirmAsync(UiText.Get("替换滚轮组合绑定"),UiText.Get("将替换后退侧键的滚轮组合动作，单击、拖动及其他按键绑定保持原样。"))) return;
                await Model.SetNativeZoomAsync("back");
            }),ZoomAvailable),
            Number(UiText.Get("缩放速度（倍）"),c => Math.Abs(c.Zoom.MmPerNotch)/2,(c,v) => c.Zoom.MmPerNotch = Math.CopySign(v*2,c.Zoom.MmPerNotch),0.0000005,50,ZoomAvailable),
            Reverse(UiText.Get("反转缩放方向"),c => c.Zoom.MmPerNotch,(c,v) => c.Zoom.MmPerNotch = v,ZoomAvailable))));
        PageContent.Children.Add(Card(Stack(Text(UiText.Get("拖动内容")),Number(UiText.Get("水平速度"),c => Math.Abs(c.Content.MmPerCountX),(c,v) => c.Content.MmPerCountX = Math.CopySign(v,c.Content.MmPerCountX)),Number(UiText.Get("垂直速度"),c => Math.Abs(c.Content.MmPerCountY),(c,v) => c.Content.MmPerCountY = Math.CopySign(v,c.Content.MmPerCountY)),Reverse(UiText.Get("反转水平方向"),c => c.Content.MmPerCountX,(c,v) => c.Content.MmPerCountX = v),Reverse(UiText.Get("反转垂直方向"),c => c.Content.MmPerCountY,(c,v) => c.Content.MmPerCountY = v))));
        var fingers = RequiresHost(new ComboBox { Header = UiText.Get("Windows 手势指数组"), ItemsSource = new[] {UiText.Get("三指"),UiText.Get("四指")} }); _refresh.Add(c => fingers.SelectedIndex = c.Shell.Fingers == 3 ? 0 : 1);
        fingers.SelectionChanged += (_,_) => { if (!_updating) Model.Edit(c => c.Shell.Fingers = fingers.SelectedIndex == 0 ? 3u : 4u); };
        PageContent.Children.Add(Card(Stack(Text(UiText.Get("系统手势")),fingers,Number(UiText.Get("左右桌面切换速度"),c => Math.Abs(c.Shell.MmPerCountX),(c,v) => c.Shell.MmPerCountX = Math.CopySign(v,c.Shell.MmPerCountX)),Number(UiText.Get("任务视图与显示桌面速度"),c => Math.Abs(c.Shell.MmPerCountY),(c,v) => c.Shell.MmPerCountY = Math.CopySign(v,c.Shell.MmPerCountY)),Reverse(UiText.Get("反转桌面切换方向"),c => c.Shell.MmPerCountX,(c,v) => c.Shell.MmPerCountX = v),Reverse(UiText.Get("反转上下手势方向"),c => c.Shell.MmPerCountY,(c,v) => c.Shell.MmPerCountY = v),Text(UiText.Get("请与 Windows 中的三指或四指手势设置保持一致。")),Button(UiText.Get("打开 Windows 触摸板设置"),async () => { await Launcher.LaunchUriAsync(new("ms-settings:devices-touchpad")); }))));
        var mode = RequiresHost(new ComboBox { Header = UiText.Get("普通滚轮增强") }); foreach(var label in new[]{UiText.Get("关闭"),UiText.Get("标准平滑"),UiText.Get("高平滑")}) mode.Items.Add(new ComboBoxItem { Content = label });
        _refresh.Add(c => { mode.SelectedIndex = c.Wheel.Mode == "off" ? 0 : c.Wheel.Mode == "regular" ? 1 : 2; bool supported = Model.Status.Capabilities.Any(x => x.Name == "wheel" && x.State == "supported"); ((ComboBoxItem)mode.Items[1]).IsEnabled = ((ComboBoxItem)mode.Items[2]).IsEnabled = supported; });
        mode.SelectionChanged += (_,_) => { if (!_updating) Model.Edit(c => c.Wheel.Mode = mode.SelectedIndex == 0 ? "off" : mode.SelectedIndex == 1 ? "regular" : "high"); };
        _capability = Text(""); PageContent.Children.Add(Card(Stack(mode,_capability,Number(UiText.Get("每格滚动量"),c => Math.Abs(c.Wheel.MmPerNotch),(c,v) => c.Wheel.MmPerNotch = Math.CopySign(v,c.Wheel.MmPerNotch),available: WheelAvailable),Reverse(UiText.Get("反转滚轮方向"),c => c.Wheel.MmPerNotch,(c,v) => c.Wheel.MmPerNotch = v,WheelAvailable))));
        var advanced = Stack(
            AdvancedPair(UiText.Get("手势识别"),Number(UiText.Get("拖动激活阈值（相对计数）"),c => c.Content.ThresholdCounts,(c,v) => c.Content.ThresholdCounts = v,0.000001,1000000),Number(UiText.Get("方向优势轴比例"),c => c.Shell.DirectionRatio,(c,v) => c.Shell.DirectionRatio = v,1.01,8)),Separator(),
            AdvancedPair(UiText.Get("按键时序"),Number(UiText.Get("双击等待（毫秒）"),c => c.Timing.DoubleClickMs,(c,v) => c.Timing.DoubleClickMs = (uint)v,100,2000),Number(UiText.Get("按住等待（毫秒）"),c => c.Timing.HoldMs,(c,v) => c.Timing.HoldMs = (uint)v,100,5000)),Separator(),
            AdvancedPair(UiText.Get("滚轮平滑"),Number(UiText.Get("标准平滑时长（毫秒）"),c => c.Wheel.RegularMilliseconds,(c,v) => c.Wheel.RegularMilliseconds = v,10,500,WheelAvailable),Number(UiText.Get("高平滑时长（毫秒）"),c => c.Wheel.HighMilliseconds,(c,v) => c.Wheel.HighMilliseconds = v,10,1000,WheelAvailable)));
        advanced.Margin = new(0,12,0,0);
        _advancedSettings = new Expander { Header = UiText.Get("高级参数"), Content = advanced, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch }; PageContent.Children.Add(_advancedSettings);
    }
    private HyperlinkButton AboutLink(string label,string glyph,Func<Task> action) {
        var icon = new FontIcon { Glyph = glyph, FontSize = 22, Width = 28 };
        var caption = Text(label); caption.FontSize = 16; caption.VerticalAlignment = VerticalAlignment.Center;
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 }; content.Children.Add(icon); content.Children.Add(caption);
        var link = new HyperlinkButton { Content = content, Padding = new(6,10,6,10), HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left };
        AutomationProperties.SetName(link,label);
        void Colors() { icon.Foreground = link.Foreground; caption.Foreground = link.Foreground; }
        link.Loaded += (_,_) => Colors(); link.ActualThemeChanged += (_,_) => Colors();
        link.Click += async (_,_) => { try { await action(); } catch(Exception error) { _uiError = error.Message; RefreshState(); } };
        return link;
    }
    private void About() {
        var name = Text("Win Mouse Fix",true); name.FontSize = 25; name.HorizontalAlignment = HorizontalAlignment.Center;
        var version = Text(UiText.Format("版本：{0}","v"+CurrentProductVersion)); version.FontSize = 17; version.HorizontalAlignment = HorizontalAlignment.Center;
        var author = Secondary(UiText.Get("EEEEhex 制作")); author.HorizontalAlignment = HorizontalAlignment.Center;
        var hero = Stack(new Image { Source = new BitmapImage(new("ms-appx:///Assets/ProductIcon.png")), Width = 88, Height = 88, HorizontalAlignment = HorizontalAlignment.Center, Margin = new(0,8,0,14) },name,version,author);
        hero.Spacing = 9; hero.Margin = new(0,0,0,22); PageContent.Children.Add(hero); PageContent.Children.Add(Separator());
        var links = new Grid { ColumnSpacing = 20, RowSpacing = 12, Margin = new(16,6,16,6) };
        links.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) }); links.ColumnDefinitions.Add(new() { Width = new(1,GridUnitType.Star) });
        for(int i = 0; i < 3; ++i) links.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var entries = new[]{
            AboutLink(UiText.Get("提交反馈报告"),"\uE90A",async () => { await Launcher.LaunchUriAsync(new(RepositoryUrl+"/issues/new")); }),
            AboutLink(UiText.Get("在 GitHub 查看"),"\uE943",async () => { await Launcher.LaunchUriAsync(new(RepositoryUrl)); }),
            AboutLink(UiText.Get("使用帮助"),"\uE897",HelpAsync),
            AboutLink(UiText.Get("导出诊断报告"),"\uE9F9",ExportDiagnosticsAsync),
            AboutLink(UiText.Get("开源许可"),"\uE8A5",async () => { await Launcher.LaunchUriAsync(new(RepositoryUrl+"/blob/main/LICENSE")); }),
            AboutLink(UiText.Get("鸣谢与参考"),"\uE734",AcknowledgementsAsync)
        };
        for(int i = 0; i < entries.Length; ++i) { Grid.SetColumn(entries[i],i%2); Grid.SetRow(entries[i],i/2); links.Children.Add(entries[i]); }
        PageContent.Children.Add(links); PageContent.Children.Add(Separator());
        var footer = Secondary(UiText.Get("适用于 Windows 11 · 64 位")); footer.HorizontalAlignment = HorizontalAlignment.Center; footer.Margin = new(0,4,0,0); PageContent.Children.Add(footer);
    }
    private async Task AcknowledgementsAsync() {
        var panel = Stack(Text(UiText.Get("界面与交互参考 Mac Mouse Fix，Windows 输入引擎独立实现。")),
            AboutLink("Mac Mouse Fix","\uE8A7",async () => { await Launcher.LaunchUriAsync(new("https://github.com/noah-nuebling/mac-mouse-fix")); }),
            Text(UiText.Get("使用 Windows App SDK、.NET 与 Windows SDK，相关许可随发布包提供。")));
        await new ContentDialog { XamlRoot = Root.XamlRoot, Title = UiText.Get("鸣谢与参考"), Content = panel, CloseButtonText = UiText.Get("知道了") }.ShowAsync();
    }
    private void RefreshControls() { if (Model is null) return; _updating = true; try { foreach(var update in _refresh) update(Model.Draft); RefreshBindings(); } finally { _updating = false; } }
    private void RefreshState() {
        if (!_endpoint.Simulation && Model.Status.ExitRequested && !_closing) { _hostLost = true; Close(); return; }
        if (Model is null) return; ConnectionBar.Message = _uiError ?? Model.Message; ConnectionBar.Severity = _uiError is not null || !Model.Connected || Model.Issue is not null || Model.Status.StartupError != 0 || Model.Status.LogError != 0 ? InfoBarSeverity.Warning : Model.Saving ? InfoBarSeverity.Informational : InfoBarSeverity.Success;
        ConnectionBar.IsOpen = ConnectionBar.Severity != InfoBarSeverity.Success;
        foreach(var item in _hostControls) item.Key.IsEnabled = Model.Connected && !_closing && item.Value();
        if (_bindingEditor is not null && _bindingDialog is not null) { _bindingEditor.SetIdentificationState(Model.Status.Recording); _bindingEditor.IsEnabled = Model.Connected && !Model.Saving; _bindingDialog.IsPrimaryButtonEnabled = Model.Connected && !Model.Saving && !_bindingEditor.ShortcutRecording; }
        RefreshBindingAvailability();
        if (_lastConnected != Model.Connected) { _lastConnected = Model.Connected; RefreshBindings(); }
        _updating = true;
        if (_enabled is not null) { _enabled.IsOn = Model.Status.Requested; _enabled.IsEnabled = Model.Connected && !_closing; }
        if (_runtime is not null) _runtime.Text = Model.Status.State switch { "enabled" => UiText.Get("后台已启用"), "starting" => UiText.Get("正在启动输入引擎…"), "faulted" => UiText.Format("后台发生错误，已停止接管。错误 {0}，清理 {1}",Model.Status.Failure.NativeError,Model.Status.Failure.CleanupError), "stopping" => UiText.Get("正在退出，请松开仍按住的侧键。"), _ => UiText.Get("后台已暂停，普通输入保持原样。") };
        if (_capability is not null) _capability.Text = Model.Status.Capabilities.FirstOrDefault(c => c.Name == "wheel")?.State switch { "supported" => UiText.Get("滚轮增强可用。"), "unavailable" => UiText.Get("当前系统不支持滚轮增强。"), _ => UiText.Get("滚轮增强暂不可用，普通滚轮保持原有行为。") };
        if (_identifyButton is not null && Model.Status.RecordedButton is { } found && found.Serial > _lastIdentifiedSerial) { _lastIdentifiedSerial = found.Serial; _identifyButton.SelectedIndex = found.Button == "back" ? 0 : found.Button == "forward" ? 1 : found.Button == "middle" ? 2 : -1; if (_identifyModifiers is not null) for(int i = 0; i < 4; ++i) _identifyModifiers[i].IsChecked = (found.Modifiers & (1u<<i)) != 0; }
        _updating = false;
        TryShowUpdateReminder();
    }
    private async Task EditBindingAsync(ButtonBinding? existing) {
        if (!Model.Connected || !await Model.FlushAsync()) return;
        ExpandBindingWindow();
        await SettleWindowLayoutAsync();
        var draft = existing is null ? new ButtonBinding { Id = Enumerable.Range(1,65).Select(i => (uint)i).First(i => Model.Draft.Bindings.All(b => b.Id != i)) } : JsonSerializer.Deserialize<ButtonBinding>(JsonSerializer.Serialize(existing,ConfigurationJson.Options),ConfigurationJson.Options)!;
        var editor = new Controls.BindingEditor();
        editor.MouseButton.ItemsSource = new[]{UiText.Get("后退侧键"),UiText.Get("前进侧键"),UiText.Get("中键")}; editor.MouseButton.SelectedIndex = draft.Button == "back" ? 0 : draft.Button == "forward" ? 1 : 2;
        var names = new[]{"click","double-click","hold","drag","wheel","wheel-up","wheel-down"}; editor.Trigger.ItemsSource = names.Select(TriggerName).ToArray(); editor.Trigger.SelectedIndex = Array.IndexOf(names,draft.Trigger);
        for(int i = 0; i < 4; ++i) editor.Modifiers[i].IsChecked = (draft.Modifiers & (1u<<i)) != 0;
        editor.SetKeys(draft.Keys);
        void Actions() {
            var preferred = editor.Action.SelectedItem is ComboBoxItem current ? (string)current.Tag : draft.Action;
            var trigger = names[Math.Max(0,editor.Trigger.SelectedIndex)]; editor.SetTriggerNote(trigger); editor.Action.Items.Clear();
            foreach(var action in BindingActionOptions(trigger)) editor.Action.Items.Add(new ComboBoxItem { Content = action == "shortcut" ? UiText.Get("发送快捷键") : BindingActionName(action), Tag = action, IsEnabled = action != "native-zoom" || ZoomAvailable() });
            editor.Action.SelectedItem = editor.Action.Items.OfType<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == preferred) ?? editor.Action.Items[0];
        }
        Actions(); editor.Trigger.SelectionChanged += (_,_) => Actions(); editor.RecordingChanged += RefreshState;
        editor.IdentifyButton.Click += async (_,_) => { editor.StopRecording(); _lastIdentifiedSerial = 0; await Model.RecordAsync(!Model.Status.Recording); };
        var title = Text(existing is null ? UiText.Get("添加绑定") : UiText.Get("编辑绑定"),true); title.FontSize = 24;
        var header = Stack(title,Secondary(UiText.Get("设置鼠标按键的触发条件和执行动作"))); header.Spacing = 4;
        var scroller = new ScrollViewer { Content = editor, HorizontalContentAlignment = HorizontalAlignment.Stretch, HorizontalScrollMode = ScrollMode.Disabled, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        var body = new Grid { RowSpacing = 12 }; body.RowDefinitions.Add(new() { Height = new(1,GridUnitType.Star) }); body.RowDefinitions.Add(new() { Height = GridLength.Auto }); body.Children.Add(scroller);
        var hint = Secondary(UiText.Get("更改后点击保存，绑定才会生效。")); hint.FontSize = 12; Grid.SetRow(hint,1); body.Children.Add(hint);
        var dialog = new ContentDialog { Title = new Border { Child = header, BorderThickness = new(0,0,0,1), BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"], Padding = new(0,0,0,14) }, Content = body, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            PrimaryButtonText = UiText.Get("保存绑定"), CloseButtonText = UiText.Get("取消"), DefaultButton = ContentDialogButton.Primary, XamlRoot = Root.XamlRoot, RequestedTheme = Root.RequestedTheme };
        dialog.Resources["ContentDialogMaxWidth"] = 780.0; dialog.Resources["ContentDialogTitleMaxHeight"] = 120.0;
        double TopInset() => Math.Max(48,TitleBar.ActualHeight)+8;
        void Resize() { dialog.Width = Math.Max(0,Math.Min(780,Root.ActualWidth-32)); header.Width = Math.Max(0,dialog.Width-48); scroller.MaxHeight = Math.Max(0,Root.ActualHeight-TopInset()-230); }
        void ArrangeCommands() {
            if (FindVisual<Border>(dialog,item => item.Name == "BackgroundElement") is { } background) {
                var margin = new Thickness(0,TopInset(),0,8); double maximum = Math.Max(0,Root.ActualHeight-margin.Top-margin.Bottom);
                if (background.Margin != margin) background.Margin = margin;
                if (background.MinHeight != 0) background.MinHeight = 0;
                if (background.MaxHeight != maximum) background.MaxHeight = maximum;
            }
            if (FindVisual<Grid>(dialog,item => item.Name == "CommandSpace") is { } commands) {
                if (commands.FlowDirection != FlowDirection.RightToLeft) commands.FlowDirection = FlowDirection.RightToLeft;
                if (commands.HorizontalAlignment != HorizontalAlignment.Right) commands.HorizontalAlignment = HorizontalAlignment.Right;
                if (commands.Width != 320) commands.Width = 320;
                foreach(var name in new[]{"PrimaryButton","CloseButton"}) if (FindVisual<Button>(commands,item => item.Name == name) is { } button && button.FlowDirection != FlowDirection.LeftToRight) button.FlowDirection = FlowDirection.LeftToRight;
            }
        }
        EventHandler<object> footerLayout = (_,_) => ArrangeCommands(); dialog.LayoutUpdated += footerLayout;
        SizeChangedEventHandler resize = (_,_) => Resize(); Resize(); Root.SizeChanged += resize;
        _bindingDialog = dialog; _bindingEditor = editor; _identifyButton = editor.MouseButton; _identifyModifiers = editor.Modifiers; _lastIdentifiedSerial = 0; RefreshState();
        dialog.PrimaryButtonClick += async (_,args) => {
            var deferral = args.GetDeferral();
            try {
                await Model.RecordAsync(false);
                if (editor.ShortcutRecording) { args.Cancel = true; editor.ShowError(UiText.Get("请先完成或取消快捷键录制。")); return; }
                if (editor.MouseButton.SelectedIndex < 0) { args.Cancel = true; editor.ShowError(UiText.Get("未识别的标准按钮，请重新识别或选择。")); return; }
                draft.Button = editor.MouseButton.SelectedIndex == 0 ? "back" : editor.MouseButton.SelectedIndex == 1 ? "forward" : "middle"; draft.Trigger = names[Math.Max(0,editor.Trigger.SelectedIndex)]; draft.Modifiers = 0;
                for(int i = 0; i < 4; ++i) if (editor.Modifiers[i].IsChecked == true) draft.Modifiers |= 1u<<i;
                if (editor.Action.SelectedItem is not ComboBoxItem selected || !selected.IsEnabled) { args.Cancel = true; editor.ShowError(UiText.Get("该原生能力尚未验证，暂不能绑定。")); return; }
                draft.Action = (string)selected.Tag; draft.Keys = draft.Action == "shortcut" ? editor.ShortcutKeys.ToList() : [];
                var candidate = Model.Draft.Clone(); candidate.Bindings.RemoveAll(b => b.Id == draft.Id); candidate.Bindings.Add(draft); var issue = ConfigurationJson.Validate(candidate);
                if (issue is not null) { args.Cancel = true; editor.ShowError(issue.Message); return; }
                if (!await Model.SaveBindingAsync(draft)) { args.Cancel = true; editor.ShowError(Model.Message); }
            } catch(Exception error) { args.Cancel = true; editor.ShowError(error.Message); }
            finally { deferral.Complete(); }
        };
        try { await dialog.ShowAsync(); }
        finally { Root.SizeChanged -= resize; dialog.LayoutUpdated -= footerLayout; editor.RecordingChanged -= RefreshState; editor.StopRecording(); await Model.RecordAsync(false); _bindingDialog = null; _bindingEditor = null; _identifyButton = null; _identifyModifiers = null; }
    }
    private void ExpandBindingWindow() {
        if (AppWindow.Presenter is not OverlappedPresenter presenter || presenter.State != OverlappedPresenterState.Restored) return;
        var position = AppWindow.Position; var size = AppWindow.Size;
        var work = DisplayArea.GetFromWindowId(AppWindow.Id,DisplayAreaFallback.Nearest).WorkArea;
        double scale = Root.XamlRoot.RasterizationScale;
        int width = Math.Max(size.Width,Math.Min(work.Width,(int)Math.Ceiling(880*scale)));
        int height = Math.Max(size.Height,Math.Min(work.Height,(int)Math.Ceiling(860*scale)));
        if (width == size.Width && height == size.Height) return;
        int x = width <= work.Width ? Math.Clamp(position.X-(width-size.Width)/2,work.X,work.X+work.Width-width) : position.X;
        int y = height <= work.Height ? Math.Clamp(position.Y-(height-size.Height)/2,work.Y,work.Y+work.Height-height) : position.Y;
        AppWindow.MoveAndResize(new(x,y,width,height));
    }
    private Task SettleWindowLayoutAsync() {
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low,() => { try { Root.UpdateLayout(); ready.SetResult(); } catch(Exception error) { ready.SetException(error); } })) ready.SetException(new InvalidOperationException("Window dispatcher is closing."));
        return ready.Task;
    }
    private static string ButtonName(string name) => name switch { "back" => UiText.Get("后退侧键"), "forward" => UiText.Get("前进侧键"), "middle" => UiText.Get("中键"), _ => UiText.Get("未知按钮") };
    private static string TriggerName(string name) => name switch { "click" => UiText.Get("单击"), "double-click" => UiText.Get("双击"), "hold" => UiText.Get("按住"), "drag" => UiText.Get("按住并移动"), "wheel" => UiText.Get("按住并滚轮"), "wheel-up" => UiText.Get("按住并向上滚轮"), _ => UiText.Get("按住并向下滚轮") };
    private static string ActionName(string name) => name switch { "back" => UiText.Get("后退"), "forward" => UiText.Get("前进"), "middle-click" => UiText.Get("中键点击"), "content-drag" => UiText.Get("二维拖动内容"), "shell-gestures" => UiText.Get("任务视图 / 显示桌面 / 切换桌面"), "native-zoom" => UiText.Get("原生缩放"), "shortcut" => UiText.Get("快捷键"), "volume-up" => UiText.Get("增大音量"), "volume-down" => UiText.Get("减小音量"), "mute" => UiText.Get("静音"), "play-pause" => UiText.Get("播放 / 暂停"), "previous-track" => UiText.Get("上一首"), _ => UiText.Get("下一首") };
    private static string Modifiers(uint value) => string.Join(" + ",new[]{"Shift","Ctrl","Alt","Win"}.Where((_,i) => (value & (1u<<i)) != 0)) is {Length:>0} text ? text+" · " : "";
    private static string KeyName(uint key) => key switch { 16 => "Shift", 17 => "Ctrl", 18 => "Alt", 91 => "Win", _ => key is >= 48 and <= 90 ? ((char)key).ToString() : ((VirtualKey)key).ToString() };
    private async void Root_KeyDown(object sender,KeyRoutedEventArgs e) { if (e.Key == VirtualKey.Escape && Model.Status.Recording) { e.Handled = true; await Model.RecordAsync(false); _bindingDialog?.Hide(); } }
    private async void Reload_Click(object sender,RoutedEventArgs e) { _uiError = null; await Model.ReloadAsync(); RefreshControls(); }
    private async Task<bool> ConfirmAsync(string title,string message) => await new ContentDialog { XamlRoot = Root.XamlRoot, Title = title, Content = Text(message), PrimaryButtonText = UiText.Get("确认替换"), CloseButtonText = UiText.Get("取消"), DefaultButton = ContentDialogButton.Close }.ShowAsync() == ContentDialogResult.Primary;
    private async Task ImportAsync() { if (!Model.Connected) return; var picker = new FileOpenPicker(); picker.FileTypeFilter.Add(".json"); WinRT.Interop.InitializeWithWindow.Initialize(picker,WinRT.Interop.WindowNative.GetWindowHandle(this)); var file = await picker.PickSingleFileAsync(); if (file is null) return;
        try { var bytes = await File.ReadAllBytesAsync(file.Path); if (bytes.Length > ConfigurationJson.MaximumBytes) throw new InvalidDataException(UiText.Get("配置文件超过 64 KiB。")); var config = ConfigurationJson.Parse(new UTF8Encoding(false,true).GetString(bytes)); if (await ConfirmAsync(UiText.Get("导入设置"),UiText.Format("将替换全部绑定、手感参数、启用及登录启动意图。导入包含 {0} 条绑定，可取消。",config.Bindings.Count))) await Model.ReplaceAsync(config); }
        catch(Exception error) { await new ContentDialog { XamlRoot = Root.XamlRoot, Title = UiText.Get("无法导入"), Content = error.Message, CloseButtonText = UiText.Get("关闭") }.ShowAsync(); } }
    private async Task ExportAsync() { if (!Model.Connected) return; await Model.ReloadAsync(); var picker = SavePicker("WinMouseFix-settings",UiText.Get("JSON 设置"),".json"); var file = await picker.PickSaveFileAsync(); if (file is not null) await File.WriteAllTextAsync(file.Path,ConfigurationJson.Serialize(Model.Committed),new UTF8Encoding(false)); }
    private FileSavePicker SavePicker(string name,string type,string extension) { var picker = new FileSavePicker { SuggestedFileName = name }; picker.FileTypeChoices.Add(type,new[]{extension}); WinRT.Interop.InitializeWithWindow.Initialize(picker,WinRT.Interop.WindowNative.GetWindowHandle(this)); return picker; }
    private async Task ResetAsync() { if (Model.Connected && await ConfirmAsync(UiText.Get("恢复默认"),UiText.Get("将恢复默认绑定与手感参数，暂停增强并关闭登录启动。已有损坏配置会另存保留。"))) await Model.ReplaceAsync(new(),true); }
    private async Task ExportDiagnosticsAsync() { var source = await Model.ExportDiagnosticsAsync(); if (source is null) return; var picker = SavePicker("WinMouseFix-diagnostics",UiText.Get("诊断归档"),".zip"); var file = await picker.PickSaveFileAsync(); if (file is not null) File.Copy(source,file.Path,true); }
    private async Task HelpAsync() { await new ContentDialog { XamlRoot = Root.XamlRoot, Title = UiText.Get("使用帮助"), Content = Text(UiText.Get("在“通用”中启动增强，在“按键”中配置单击、拖动或滚轮动作。\n\n在“滚动”中调整速度和方向。需要暂停或退出时，使用系统托盘菜单。\n\n关闭设置窗口后后台继续运行；退出后台时设置窗口也会关闭。")), CloseButtonText = UiText.Get("知道了") }.ShowAsync(); }
    public async Task RunSmokeAsync(string directory) {
        if (!(_endpoint.Simulation && Model.Connected)) throw new InvalidOperationException("UI smoke requires the explicit fake session.");
        Root.RequestedTheme = ElementTheme.Light;
        foreach(var page in new[]{"general","buttons","scrolling","about"}) {
            Navigate(page); await Task.Delay(150); Root.UpdateLayout();
            if (page == "general") {
                var before = Model.Committed.AutomaticUpdates; _automaticUpdates!.IsOn = !before; await Task.Delay(150); await Model.ReloadAsync();
                if (Model.Committed.AutomaticUpdates != !before) throw new InvalidOperationException("Update preference was not persisted through Host ACK/reload.");
                _automaticUpdates.IsOn = before; await Task.Delay(150);
                var noticeBefore = Model.Committed.StartupNotification; _startupNotice!.IsOn = !noticeBefore; await Task.Delay(150); await Model.ReloadAsync();
                if (Model.Committed.StartupNotification != !noticeBefore || _startupNotice.IsOn != !noticeBefore) throw new InvalidOperationException("Startup notification preference did not persist through Host ACK/reload.");
                _startupNotice.IsOn = noticeBefore; await Task.Delay(150);
            }
            if (page == "buttons") {
                var selector = _bindingActions[1]; selector.SelectedItem = selector.Items.OfType<ComboBoxItem>().First(i => (string)i.Tag == "forward"); await Task.Delay(200);
                if (Model.Committed.Bindings.First(b => b.Id == 1).Action != "forward" || !ReferenceEquals(selector,_bindingActions[1])) throw new InvalidOperationException("Inline binding edit did not preserve its row and receive an ACK.");
                ((FakeHostSession)_session).RejectNext = true; selector.SelectedItem = selector.Items.OfType<ComboBoxItem>().First(i => (string)i.Tag == "volume-up"); await Task.Delay(200);
                if (Model.Committed.Bindings.First(b => b.Id == 1).Action != "forward" || Model.Issue is null) throw new InvalidOperationException("Rejected inline binding edit was shown as committed.");
                await Model.ReloadAsync(); await ChangeBindingActionAsync(1,"back"); await Task.Delay(200);
            }
            foreach(var child in PageContent.Children.OfType<FrameworkElement>()) {
                var origin = child.TransformToVisual(Root).TransformPoint(new(0,0));
                if (origin.X < -1 || origin.X+child.ActualWidth > Root.ActualWidth+1) throw new InvalidOperationException("Page content is clipped horizontally: "+page);
            }
            await CaptureAsync(Root,Path.Combine(directory,page+".png"));
            if (page == "scrolling") { PageScroll.ChangeView(null,PageScroll.ScrollableHeight,null,true); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"scrolling-end.png")); await CaptureAdvancedAsync(directory,"advanced.png"); _advancedSettings!.IsExpanded = false; PageScroll.ChangeView(null,0,null,true); }
            if (page == "general") { _moreSettings!.IsExpanded = true; await Task.Delay(350); PageScroll.ChangeView(null,PageScroll.ScrollableHeight,null,true); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"general-more.png")); _moreSettings.IsExpanded = false; PageScroll.ChangeView(null,0,null,true); await Task.Delay(200); }
        }
        await RunUpdateSmokeAsync(directory);
        await RunBindingEditorSmokeAsync(directory);
        Model.Edit(c => c.Content.MmPerCountX = 0.25); if (!await Model.FlushAsync() || Model.Committed.Content.MmPerCountX != 0.25) throw new InvalidOperationException("UI configuration did not reach fake Host ACK.");
        await Model.RecordAsync(true); await Model.RecordAsync(false);
        Root.RequestedTheme = ElementTheme.Dark; Navigate("general"); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"general-dark.png"));
        Navigate("buttons"); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"buttons-dark.png"));
        Navigate("about"); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"about-dark.png"));
        Navigate("general");
        UiText.Language = "en"; LocalizeNavigation(); BuildPage(); await Model.ReloadAsync(); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"general-english.png"));
        _moreSettings!.IsExpanded = true; await Task.Delay(350); PageScroll.ChangeView(null,PageScroll.ScrollableHeight,null,true); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"general-more-english.png"));
        Navigate("buttons"); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"buttons-english.png"));
        var previousSize = AppWindow.Size; double scale = Root.XamlRoot.RasterizationScale; AppWindow.Resize(new((int)(640*scale),(int)(760*scale))); await Task.Delay(200); await CaptureAsync(Root,Path.Combine(directory,"buttons-narrow.png")); AppWindow.Resize(previousSize); await Task.Delay(150);
        Navigate("scrolling"); await Task.Delay(150); await CaptureAdvancedAsync(directory,"advanced-english.png");
        AppWindow.Resize(new((int)(640*scale),(int)(760*scale))); await Task.Delay(200); await CaptureAdvancedAsync(directory,"advanced-narrow.png"); AppWindow.Resize(previousSize); await Task.Delay(150);
        Navigate("buttons"); AppWindow.Resize(new((int)(640*scale),(int)(760*scale))); await Task.Delay(150);
        var editorPreview = EditBindingAsync(null); await Task.Delay(250); CheckModalChrome(directory,"english-autosized");
        AppWindow.Resize(new((int)(640*scale),(int)(480*scale))); await Task.Delay(200); CheckModalChrome(directory,"english-narrow"); await CaptureAsync(BindingDialogVisual(),Path.Combine(directory,"binding-add-english-narrow.png"));
        _bindingDialog!.Hide(); await editorPreview; CheckTitleBarUnchanged();
        if (AppWindow.Size.Width != (int)(640*scale) || AppWindow.Size.Height != (int)(480*scale)) throw new InvalidOperationException("User resize was overwritten after editing.");
        AppWindow.Resize(previousSize); await Task.Delay(150);
        Navigate("about"); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,"about-english.png"));
        UiText.Language = "zh-Hans";
        File.WriteAllText(Path.Combine(directory,"result.json"),JsonSerializer.Serialize(new { simulation = true, ok = true, pages = 4, configurationAck = true, nativeCapture = false, desktopInjection = false }));
    }
    private async Task CaptureAdvancedAsync(string directory,string name) {
        _advancedSettings!.IsExpanded = true; await Task.Delay(350); PageScroll.ChangeView(null,PageScroll.ScrollableHeight,null,true); await Task.Delay(150); await CaptureAsync(Root,Path.Combine(directory,name));
    }
    private static T? FindVisual<T>(DependencyObject root,Func<T,bool> match) where T:DependencyObject {
        if (root is T item && match(item)) return item;
        for(int i = 0; i < VisualTreeHelper.GetChildrenCount(root); ++i) if (FindVisual(VisualTreeHelper.GetChild(root,i),match) is { } found) return found;
        return null;
    }
    private FrameworkElement BindingDialogVisual() => FindVisual<FrameworkElement>(_bindingDialog!,item => item.Name == "BackgroundElement") ?? _bindingDialog!;
    private void InvokeBindingSave() {
        var button = FindVisual<Button>(_bindingDialog!,item => item.Name == "PrimaryButton") ?? throw new InvalidOperationException("Save button was not rendered.");
        var peer = new Microsoft.UI.Xaml.Automation.Peers.ButtonAutomationPeer(button);
        ((Microsoft.UI.Xaml.Automation.Provider.IInvokeProvider)peer.GetPattern(Microsoft.UI.Xaml.Automation.Peers.PatternInterface.Invoke)).Invoke();
    }
    private void CheckModalChrome(string directory,string sample) {
        if (!_endpoint.Simulation || _bindingDialog is null || TitleBar.Visibility != Visibility.Visible || Root.RowDefinitions[0].Height.Value != 48) throw new InvalidOperationException("Modal title bar mode was changed.");
        var metrics = ModalChromeProbe.Read(this,Root,BindingDialogVisual(),TitleBar.ActualHeight);
        File.WriteAllText(Path.Combine(directory,"caption-"+sample+".json"),JsonSerializer.Serialize(metrics));
    }
    private void CheckTitleBarUnchanged() {
        if (!ExtendsContentIntoTitleBar || TitleBar.Visibility != Visibility.Visible || Root.RowDefinitions[0].Height.Value != 48) throw new InvalidOperationException("Settings title bar mode changed during the dialog.");
    }
    private async Task RunBindingEditorSmokeAsync(string directory) {
        if (!_endpoint.Simulation || _session is not FakeHostSession fake) throw new InvalidOperationException("Binding UI smoke requires the fake session.");
        Navigate("buttons"); var original = ConfigurationJson.Serialize(Model.Draft); var originalSize = AppWindow.Size;
        var pending = EditBindingAsync(null); await Task.Delay(250);
        var editor = _bindingEditor ?? throw new InvalidOperationException("Binding editor did not open.");
        if (editor.ShortcutVisible) throw new InvalidOperationException("Shortcut recorder was shown for a normal action.");
        CheckModalChrome(directory,"normal");
        var area = DisplayArea.GetFromWindowId(AppWindow.Id,DisplayAreaFallback.Nearest).WorkArea;
        if (AppWindow.Position.X < area.X || AppWindow.Position.Y < area.Y || AppWindow.Position.X+AppWindow.Size.Width > area.X+area.Width || AppWindow.Position.Y+AppWindow.Size.Height > area.Y+area.Height) throw new InvalidOperationException("Automatic window growth exceeded the monitor work area.");
        var expandedSize = AppWindow.Size;
        await CaptureAsync(BindingDialogVisual(),Path.Combine(directory,"binding-add.png"));
        var windowSize = AppWindow.Size; double scale = Root.XamlRoot.RasterizationScale;
        AppWindow.Resize(new((int)(640*scale),(int)(480*scale))); await Task.Delay(250); CheckModalChrome(directory,"small"); await CaptureAsync(BindingDialogVisual(),Path.Combine(directory,"binding-add-small.png")); AppWindow.Resize(windowSize); await Task.Delay(200);
        InvokeBindingSave(); await Task.Delay(150);
        if (!editor.HasError || ConfigurationJson.Serialize(Model.Draft) != original) throw new InvalidOperationException("Conflicting binding changed configuration.");
        await Model.RecordAsync(true); fake.Identify("middle",2); await Task.Delay(100);
        if (editor.MouseButton.SelectedIndex != 2 || editor.Modifiers[1].IsChecked != true) throw new InvalidOperationException("Detected button/modifiers did not reach the editor.");
        editor.Action.SelectedItem = editor.Action.Items.OfType<ComboBoxItem>().First(item => (string)item.Tag == "shortcut"); editor.SetKeys([16,75]); await Task.Delay(150);
        if (!editor.ShortcutVisible) throw new InvalidOperationException("Shortcut action did not show its recorder.");
        await CaptureAsync(BindingDialogVisual(),Path.Combine(directory,"binding-shortcut.png"));
        fake.RejectNext = true; InvokeBindingSave(); await Task.Delay(150);
        if (!editor.HasError || ConfigurationJson.Serialize(Model.Draft) != original) throw new InvalidOperationException("Rejected binding leaked into the saved draft.");
        _bindingDialog!.Hide(); await pending;
        CheckTitleBarUnchanged();
        if (AppWindow.Size.Width != expandedSize.Width || AppWindow.Size.Height != expandedSize.Height) throw new InvalidOperationException("Window size changed when cancelling the dialog.");
        if (Model.Status.Recording || !await Model.FlushAsync() || ConfigurationJson.Serialize(Model.Committed) != original) throw new InvalidOperationException("Cancelled binding was saved or identification leaked.");
        await Model.ReloadAsync(); pending = EditBindingAsync(null); await Task.Delay(200); editor = _bindingEditor!;
        editor.MouseButton.SelectedIndex = 2; editor.Modifiers[1].IsChecked = true; editor.Action.SelectedItem = editor.Action.Items.OfType<ComboBoxItem>().First(item => (string)item.Tag == "shortcut"); editor.SetKeys([16,75]);
        InvokeBindingSave(); await pending.WaitAsync(TimeSpan.FromSeconds(3));
        CheckTitleBarUnchanged();
        if (AppWindow.Size.Width != expandedSize.Width || AppWindow.Size.Height != expandedSize.Height) throw new InvalidOperationException("Window size changed when saving the dialog.");
        File.WriteAllText(Path.Combine(directory,"autosize.json"),JsonSerializer.Serialize(new { originalWidth=originalSize.Width,originalHeight=originalSize.Height,expandedWidth=expandedSize.Width,expandedHeight=expandedSize.Height,retainedAfterCancel=true,retainedAfterSave=true,titleBarModeUnchanged=true }));
        var saved = Model.Committed.Bindings.Single(b => b.Button == "middle" && b.Modifiers == 2);
        if (saved.Action != "shortcut" || !saved.Keys.SequenceEqual(new uint[]{16,75}) || Model.Status.Recording) throw new InvalidOperationException("Valid binding was not committed or recording was left active.");
        Model.Edit(c => c.Bindings.RemoveAll(b => b.Id == saved.Id)); if (!await Model.FlushAsync()) throw new InvalidOperationException("Fake binding cleanup failed.");
    }
    private static async Task CaptureAsync(FrameworkElement element,string path) {
        element.UpdateLayout(); var bitmap = new RenderTargetBitmap(); await bitmap.RenderAsync(element);
        if (bitmap.PixelWidth == 0 || bitmap.PixelHeight == 0) throw new InvalidOperationException("UI rendered an empty bitmap.");
        var bytes = (await bitmap.GetPixelsAsync()).ToArray();
        using var file = File.Create(path); using var stream = file.AsRandomAccessStream();
        var encoder = await Windows.Graphics.Imaging.BitmapEncoder.CreateAsync(Windows.Graphics.Imaging.BitmapEncoder.PngEncoderId,stream);
        encoder.SetPixelData(Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8,Windows.Graphics.Imaging.BitmapAlphaMode.Premultiplied,(uint)bitmap.PixelWidth,(uint)bitmap.PixelHeight,96,96,bytes); await encoder.FlushAsync();
    }
}




