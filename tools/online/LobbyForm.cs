using System;
using System.IO;
using System.Linq;
using System.Diagnostics;
using System.Drawing;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PartyBoardOnline {
sealed class MainForm : Form {
    Label status,discLabel,footer;TextBox invitation,nickname;ListView players;ComboBox playerCount;FlowLayoutPanel missingMods;
    Button host,join,copy,play,cancel,choose,paste,update;Session session;DiscFile disc;
    readonly CancellationTokenSource fileCancel=new CancellationTokenSource();bool closing,hashing;
    Report lastReport;
    public MainForm() {
        Text="PartyBoard — Salon en ligne v"+UpdateService.CurrentVersion;ClientSize=new Size(840,760);MinimumSize=new Size(800,790);
        StartPosition=FormStartPosition.CenterScreen;Font=new Font("Segoe UI",11);BackColor=Color.FromArgb(245,247,252);
        var root=new TableLayoutPanel{Dock=DockStyle.Fill,Padding=new Padding(24),ColumnCount=1,RowCount=11};
        foreach(int height in new[]{48,35,46,67,47,65,47,145})root.RowStyles.Add(new RowStyle(SizeType.Absolute,height));
        root.RowStyles.Add(new RowStyle(SizeType.Percent,100));root.RowStyles.Add(new RowStyle(SizeType.Absolute,48));root.RowStyles.Add(new RowStyle(SizeType.Absolute,30));Controls.Add(root);
        root.Controls.Add(new Label{Text="Votre salon PartyBoard",Font=new Font("Segoe UI",23,FontStyle.Bold),AutoSize=true},0,0);
        root.Controls.Add(new Label{Text="Choisissez votre pseudo et votre disque. L'hôte lancera le jeu pour tous.",Dock=DockStyle.Fill},0,1);
        var identity=new FlowLayoutPanel{Dock=DockStyle.Fill};identity.Controls.Add(new Label{Text="Votre pseudo",Width=116,Padding=new Padding(0,8,0,0)});
        nickname=new TextBox{Text="Joueur",Width=230,MaxLength=24,AccessibleName="Votre pseudo",Margin=new Padding(0,5,12,0)};identity.Controls.Add(nickname);root.Controls.Add(identity,0,2);
        var disk=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=2};disk.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,210));disk.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));
        choose=Make("Choisir mon disque",ChooseDisc);disk.Controls.Add(choose,0,0);discLabel=new Label{Text="Mario Party 4 USA, révision 1\nISO, GCM ou RVZ — vérification du fichier complet",Dock=DockStyle.Fill,Padding=new Padding(0,4,0,0)};disk.Controls.Add(discLabel,1,0);root.Controls.Add(disk,0,3);
        var top=new FlowLayoutPanel{Dock=DockStyle.Fill};
        top.Controls.Add(new Label{Text="Joueurs",Padding=new Padding(0,8,4,0)});
        playerCount=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList,Width=64,Margin=new Padding(0,3,12,3),AccessibleName="Nombre de joueurs"};
        playerCount.Items.AddRange(new object[]{"2","3","4"});playerCount.SelectedIndex=0;
        playerCount.SelectedIndexChanged+=(s,e)=>UpdateFooter();top.Controls.Add(playerCount);
        host=Make("Créer un salon",()=>Begin(true,"",SelectedPlayers()));join=Make("Rejoindre",()=>Begin(false,invitation.Text));top.Controls.Add(host);top.Controls.Add(join);root.Controls.Add(top,0,4);
        invitation=new TextBox{Dock=DockStyle.Fill,Multiline=true,ScrollBars=ScrollBars.Vertical,Font=new Font("Segoe UI",10),AccessibleName="Invitation du salon",MaxLength=220};root.Controls.Add(invitation,0,5);
        var actions=new FlowLayoutPanel{Dock=DockStyle.Fill};copy=Make("Copier l'invitation",()=>{Clipboard.SetText(invitation.Text);SetStatus("Invitation copiée. Envoyez-la à votre ami ; gardez cette fenêtre ouverte.");});
        paste=Make("Coller l'invitation",()=>{if(session==null)invitation.Text=Clipboard.GetText();});actions.Controls.Add(copy);actions.Controls.Add(paste);root.Controls.Add(actions,0,6);
        var playersRow=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=2};
        // Absolute, not percent, so the mods panel takes a fixed slice and the
        // player list keeps essentially the width it always had -- a percent
        // split shrank every column and gave the list a horizontal scrollbar
        // even when the mods panel had nothing to show.
        playersRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));playersRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,170));
        players=new ListView{Dock=DockStyle.Fill,View=View.Details,FullRowSelect=true,HeaderStyle=ColumnHeaderStyle.Nonclickable,AccessibleName="Joueurs du salon",HideSelection=false};
        players.Columns.Add("Pseudo",165);players.Columns.Add("Rôle",100);players.Columns.Add("Disque",210);players.Columns.Add("Ping",80);playersRow.Controls.Add(players,0,0);
        missingMods=new FlowLayoutPanel{Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoScroll=true,Visible=false,Padding=new Padding(12,0,0,0),AccessibleName="Mods manquants"};
        playersRow.Controls.Add(missingMods,1,0);root.Controls.Add(playersRow,0,7);
        status=new Label{Text="Choisissez votre disque pour commencer.",Dock=DockStyle.Fill,Padding=new Padding(0,10,0,0),ForeColor=Color.FromArgb(35,49,72),AccessibleName="État du salon"};root.Controls.Add(status,0,8);
        var bottom=new FlowLayoutPanel{Dock=DockStyle.Fill};
        play=Make("Lancer pour tout le monde",()=>{
            var current=session;if(current==null)return;play.Enabled=false;
            Task.Run(()=>{
                try{current.Launch();}
                catch(Exception e){UI(()=>{if(session==current){SetStatus(e.Message);RefreshLobby();}});}
            });
        });
        play.MinimumSize=new Size(240,38);
        cancel=Make("Quitter le salon",Reset);bottom.Controls.Add(play);bottom.Controls.Add(cancel);root.Controls.Add(bottom,0,9);
        actions.Controls.Add(Make("Exporter diagnostic",ExportReport));
        update=Make("Vérifier les mises à jour",CheckForUpdates);actions.Controls.Add(update);
        footer=new Label{Dock=DockStyle.Fill,Font=new Font("Segoe UI",9),ForeColor=Color.DimGray};root.Controls.Add(footer,0,10);UpdateFooter();
        nickname.TextChanged+=(s,e)=>RefreshLobby();RefreshControls();RefreshLobby();Task.Run(()=>StartupUpdateCheck());
        FormClosing+=(s,e)=>{closing=true;fileCancel.Cancel();var old=session;session=null;if(old!=null)old.Dispose();if(disc!=null)disc.Dispose();};
    }
    Button Make(string text,Action action){var b=new Button{Text=text,AutoSize=true,Height=38,MinimumSize=new Size(160,38),Margin=new Padding(0,3,12,3),FlatStyle=FlatStyle.Flat,BackColor=Color.White};b.Click+=(s,e)=>{try{action();}catch(Exception ex){SetStatus(Friendly(ex));}};return b;}
    int SelectedPlayers(){int n;return int.TryParse(playerCount.SelectedItem as string,out n)?n:2;}
    void UpdateFooter(){if(footer!=null)footer.Text=SelectedPlayers()+" joueurs · Même fichier disque requis · Ping : aller-retour entre les PC";}
    void UI(Action action){if(!closing && !IsDisposed){if(InvokeRequired){try{BeginInvoke(action);}catch(InvalidOperationException){}}else action();}}
    void SetStatus(string text){UI(()=>status.Text=text);}
    void RefreshControls(){bool idle=session==null;host.Enabled=join.Enabled=idle && disc!=null && !hashing;choose.Enabled=nickname.Enabled=idle && !hashing;paste.Enabled=idle;playerCount.Enabled=idle;cancel.Enabled=!idle;invitation.ReadOnly=!idle;
        // Two players: unchanged, Bridge appears the instant the one guest
        // connects. Above two, Bridge stays null for the whole session -- the
        // salon's own seat count is what says whether there is still room to
        // invite someone else.
        copy.Enabled=session!=null && session.Invite!=null && session.Host && (session.Lobby==null?session.Bridge==null:session.Lobby.Occupied<session.MaxPlayers);
        play.Enabled=session?.Lobby?.CanStart==true;}
    void RefreshLobby(){UI(()=>{
        if(players==null)return;RefreshControls();players.BeginUpdate();players.Items.Clear();
        var lobby=session?.Lobby;var local=lobby?.Local??session?.Profile;
        string localName=local?.Name??nickname.Text;
        bool localMatch=lobby!=null && lobby.SeatAgrees(lobby.LocalSeat);
        string localDisc=disc==null?"À choisir":localMatch?"Identique — SHA-256 vérifié":"Vérifié — attente de comparaison";
        players.Items.Add(new ListViewItem(new[]{localName,session==null?"Vous":session.Host?"Hôte (vous)":"Invité (vous)",localDisc,"Local"}));
        // Every seat this player's own Lobby actually knows about, not just
        // "the other one": a host learns every guest's profile directly, but a
        // guest's Lobby never does -- profiles are not relayed between guests,
        // only compared against the host -- so a guest's own list still shows
        // only itself and the host even at three or four players. Fixing that
        // needs its own protocol change (relaying a guest's profile the way an
        // endpoint announcement already is), not attempted here.
        bool anyRemote=false;
        if(lobby!=null) for(int seat=0;seat<Lobby.MaxSeats;seat++) {
            if(seat==lobby.LocalSeat)continue;
            var info=lobby.SeatInfo(seat);if(info==null)continue;
            anyRemote=true;bool agrees=lobby.SeatAgrees(seat);
            string seatDisc=agrees?"Identique — SHA-256 vérifié":info.DiscHash==null?"Non vérifié":"DISQUE DIFFÉRENT";
            // Per-seat ping only exists for the classic two-player Bridge; a
            // mesh session (session.Bridge stays null there) has no per-seat
            // latency measurement yet.
            string ping=session.Bridge==null?"—":session.Bridge.UdpReady && session.PingMs.HasValue?session.PingMs.Value+" ms":"En attente…";
            players.Items.Add(new ListViewItem(new[]{info.Name,seat==0?"Hôte":"Invité",seatDisc,ping}));
        }
        players.EndUpdate();
        RefreshMissingMods(lobby);
        play.Text=session!=null && !session.Host?"L'hôte lance la partie":"Lancer pour tout le monde";
        if(lobby==null)return;
        // A salon that has ended says so first. Until the peer could announce its
        // departure this branch had nothing to report, and the Running text below was
        // shown to someone whose partner had already quit.
        if(lobby.Ending==LobbyEnding.RemoteGameClosed)SetStatus("Votre ami a quitté la partie. Le salon est fermé — recréez-en un pour rejouer.");
        else if(lobby.Ending==LobbyEnding.LocalGameClosed)SetStatus("Votre partie est terminée. Le salon est fermé — recréez-en un pour rejouer.");
        else if(lobby.Phase==LobbyPhase.Preparing)SetStatus("Chargement sur les deux PC… Le jeu attendra que tout le monde soit prêt. Aucun bouton à presser dans l'autre fenêtre.");
        else if(lobby.Phase==LobbyPhase.Running)SetStatus(session.Bridge==null || session.Bridge.ControlConnected?"Partie lancée par l'hôte. Gardez le salon ouvert pendant le jeu.":"Le canal du salon est interrompu. La partie continue tant que l'autre joueur reste joignable. Gardez cette fenêtre ouverte.");
        else if(anyRemote) {
            if(!localMatch)SetStatus("Les fichiers disques sont différents : lancement bloqué. Quittez le salon, choisissez exactement le même fichier sur les deux PC, puis recréez le salon.");
            // Mods are checked after the disc because a different disc makes the mod
            // comparison meaningless, and reporting both at once helps nobody.
            else if(!lobby.ModsMatch)SetStatus((session.Host?"Un invité n'a pas les mêmes mods que vous — ":"Vos mods ne correspondent pas à ceux de l'hôte — ")+lobby.ModAdvice+". Installez-les dans CubeShelf, puis recréez le salon.");
            else SetStatus(session.Host?"Disques et mods identiques. Vous pouvez lancer la partie pour tout le monde.":"Disques et mods identiques. Attendez que l'hôte lance la partie.");
        }
    });}
    // Only a guest needs this: the host already sees who disagrees, by name,
    // through ModAdvice, and has nothing of its own to fetch. A mod already
    // present at the wrong build is not listed here either -- Missing() only
    // ever names what CubeShelf has never heard of, since a stale build is a
    // CubeShelf update, not a download link.
    void RefreshMissingMods(Lobby lobby){
        missingMods.SuspendLayout();missingMods.Controls.Clear();
        ModEntry[] missing=lobby==null || session.Host || lobby.RequiredMods==null?new ModEntry[0]:lobby.Local.Mods.Missing(lobby.RequiredMods).ToArray();
        missingMods.Visible=missing.Length>0;
        if(missing.Length>0){
            missingMods.Controls.Add(new Label{Text="Mods manquants",Font=new Font("Segoe UI",10,FontStyle.Bold),AutoSize=true,Margin=new Padding(0,0,0,6)});
            foreach(var mod in missing){
                var entry=mod;
                var row=new FlowLayoutPanel{FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoSize=true,Margin=new Padding(0,0,0,10)};
                row.Controls.Add(new Label{Text=entry.Describe(),AutoSize=true,MaximumSize=new Size(160,0)});
                var download=new Button{Text="Télécharger",AutoSize=true,Height=30,FlatStyle=FlatStyle.Flat,BackColor=Color.White,Margin=new Padding(0,3,0,0)};
                download.Click+=(s,e)=>{try{Process.Start(new ProcessStartInfo(entry.GameBananaUrl()){UseShellExecute=true});SetStatus("Page GameBanana de "+entry.Describe()+" ouverte dans votre navigateur.");}catch(Exception ex){SetStatus(Friendly(ex));}};
                row.Controls.Add(download);
                missingMods.Controls.Add(row);
            }
        }
        missingMods.ResumeLayout();
    }
    void ChooseDisc(){
        if(session!=null || hashing)return;
        using(var picker=new OpenFileDialog{Title="Choisir Mario Party 4 USA Rev 1",Filter="Images de disque|*.iso;*.gcm;*.rvz;*.wia;*.gcz;*.ciso|Tous les fichiers|*.*",CheckFileExists=true}){
            if(picker.ShowDialog(this)!=DialogResult.OK)return;string path=picker.FileName;
            if(disc!=null){disc.Dispose();disc=null;}hashing=true;RefreshLobby();SetStatus("Vérification du disque… Vous pouvez laisser cette fenêtre ouverte.");
            Task.Run(()=>{try{
                var verified=DiscFile.Verify(path,p=>UI(()=>discLabel.Text=Path.GetFileName(path)+"\nVérification SHA-256 : "+p+" %"),fileCancel.Token);
                if(closing){verified.Dispose();return;}
                UI(()=>{disc=verified;hashing=false;discLabel.Text=Path.GetFileName(path)+"\nFichier vérifié et protégé contre les modifications";SetStatus("Disque vérifié. Créez un salon ou collez l'invitation de votre ami.");RefreshLobby();});
            }catch(Exception e){UI(()=>{hashing=false;discLabel.Text="Aucun disque vérifié";SetStatus(Friendly(e));RefreshLobby();});}});
        }
    }
    void Begin(bool create,string invitationText,int players=2){
        if(session!=null || disc==null || hashing)return;
        // The mod list is read once, here, and frozen for the session. Re-reading it
        // later would let a mod be enabled between the announcement and the launch,
        // which is exactly the divergence the announcement exists to rule out.
        string modsFrom;ModSet mods;
        try {mods=ModSet.FromCubeShelf("GMPE01_00",out modsFrom);}
        catch(Exception e){SetStatus(Friendly(e));return;}
        var profile=new PlayerInfo(nickname.Text,disc.Hash,disc.Length,mods);
        Session current=null;current=new Session(t=>UI(()=>{if(session==current)SetStatus(t);}),()=>UI(()=>{if(session==current)RefreshLobby();}),t=>UI(()=>{if(session==current){Reset();SetStatus(t);}}),profile,disc);
        session=current;lastReport=current.Report;current.Host=create;RefreshLobby();
        current.Report.Write("role="+(create?"host":"guest")+" connection_requested");
        Task.Run(()=>{try{
            if(create){current.Create(players);UI(()=>{if(session==current && current.Bridge==null){invitation.Text=current.Invite.Encode();RefreshControls();SetStatus("Salon créé. Copiez l'invitation et envoyez-la à "+(players>2?"vos amis":"votre ami")+". Vous seul pourrez lancer le jeu.");}});}
            else current.Join(invitationText);
        }catch(Exception e){current.Dispose();UI(()=>{if(session==current){Reset();SetStatus(Friendly(e));}});}});
    }
    internal void PreviewLobby(){
        // Deterministic render fixture; no session, network, or selected disk.
        players.Items.Clear();players.Items.Add(new ListViewItem(new[]{"Camille","Hôte (vous)","Identique — SHA-256 vérifié","Local"}));players.Items.Add(new ListViewItem(new[]{"Alex","Invité","Identique — SHA-256 vérifié","42 ms"}));
        nickname.Text="Camille";players.Items.Clear();players.Items.Add(new ListViewItem(new[]{"Camille","Hôte (vous)","Identique — SHA-256 vérifié","Local"}));players.Items.Add(new ListViewItem(new[]{"Alex","Invité","Identique — SHA-256 vérifié","42 ms"}));
        discLabel.Text="Mario Party 4.iso\nFichier vérifié";play.Enabled=true;SetStatus("Aperçu de l'interface — joueurs et ping fictifs. Seul l'hôte peut lancer.");
    }
    void ExportReport(){
        string content=lastReport!=null?lastReport.Read():Report.ReadLatest();
        using(var save=new SaveFileDialog{Title="Enregistrer le diagnostic",Filter="Diagnostic texte|*.txt",FileName="Diagnostic-PartyBoard.txt",DefaultExt="txt",AddExtension=true}){
            if(save.ShowDialog(this)!=DialogResult.OK)return;
            File.WriteAllText(save.FileName,content,System.Text.Encoding.UTF8);
            SetStatus("Diagnostic enregistré. Envoyez ce fichier et celui de l'autre PC dans la conversation.");
        }
    }
    async void CheckForUpdates(){
        if(session!=null){SetStatus("Quittez le salon avant de mettre à jour PartyBoard.");return;}
        update.Enabled=false;SetStatus("Recherche d'une mise à jour sur GitHub…");
        try{
            var info=await UpdateService.CheckAsync();
            var current=new Version(UpdateService.CurrentVersion);
            if(info.Version<=current){SetStatus("PartyBoard est déjà à jour (v"+UpdateService.CurrentVersion+").");return;}
            var notes=String.IsNullOrWhiteSpace(info.Notes)?"Une nouvelle version est disponible.":info.Notes;
            if(MessageBox.Show(this,"La version v"+info.Version+" est disponible.\n\n"+notes+"\n\nTélécharger et installer maintenant ?","Mise à jour PartyBoard",MessageBoxButtons.YesNo,MessageBoxIcon.Information)!=DialogResult.Yes){SetStatus("Mise à jour reportée.");return;}
            SetStatus("Téléchargement de la mise à jour…");await UpdateService.InstallAsync(info);
            MessageBox.Show(this,"La mise à jour a été téléchargée. PartyBoard va redémarrer pour l'installer.","Mise à jour PartyBoard",MessageBoxButtons.OK,MessageBoxIcon.Information);
            closing=true;Application.Exit();
        }catch(Exception e){SetStatus("Impossible de vérifier la mise à jour : "+e.Message);}
        finally{if(!closing && !IsDisposed)update.Enabled=true;}
    }
    async Task StartupUpdateCheck(){
        try{
            var info=await UpdateService.CheckAsync();
            if(info.Version<=new Version(UpdateService.CurrentVersion))return;
            UI(()=>{if(update!=null && !IsDisposed){update.Text="Mise à jour disponible";update.Enabled=true;SetStatus("Une mise à jour est disponible sur GitHub. Cliquez sur le bouton de mise à jour pour l'installer.");}});
        }catch{
            // The launcher remains fully usable when GitHub is offline or blocked.
        }
    }
    static string Friendly(Exception e){if(e is IOException)return e.Message;if(e is System.ComponentModel.Win32Exception)return "Windows n'a pas donné son autorisation. Réessayez et acceptez sa demande.";if(e is OperationCanceledException)return "Vérification annulée.";return "L'opération n'a pas abouti. Vérifiez votre connexion ou recréez le salon.";}
    void Reset(){var old=session;session=null;if(old!=null)Task.Run(()=>old.Dispose());RefreshLobby();SetStatus(disc!=null?"Créez un salon ou rejoignez votre ami.":"Choisissez votre disque pour commencer.");}
}
}
