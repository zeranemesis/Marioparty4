using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Net;
using System.Diagnostics;
using System.Drawing;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PartyBoardOnline {
sealed class MainForm : Form {
    Label status,discLabel,footer;TextBox invitation,nickname;ListView players;ComboBox playerCount;FlowLayoutPanel missingMods;
    Button host,join,copy,play,cancel,choose,paste,update,mods,browse;Session session;DiscFile disc;
    // Two ways to play. On this network (LAN): nothing goes through Internet,
    // the game is found in a list of the network's games. Online: a room,
    // private (friends come by invitation from CubeShelf's friend list) or
    // public (on the relay's list, for every player with the same disc).
    RadioButton onlineMode,lanMode;ComboBox visibility;bool settingPublic;int publicNoticesSeen;
    bool Lan {get{return lanMode.Checked;}}
    bool PublicWanted {get{return !Lan && visibility.SelectedIndex==1;}}
    // Mods chosen in this window before any salon exists; null means CubeShelf's own list.
    ModSet chosenMods;
    readonly CancellationTokenSource fileCancel=new CancellationTokenSource();bool closing,hashing;
    Report lastReport;readonly Startup startup;bool startupApplied;
    public MainForm():this(Startup.Manual) {}
    public MainForm(Startup requested) {
        startup=requested??Startup.Manual;
        Text=L.T("PartyBoard — Salon en ligne v")+UpdateService.CurrentVersion;ClientSize=new Size(980,760);MinimumSize=new Size(940,790);
        StartPosition=FormStartPosition.CenterScreen;Font=new Font("Segoe UI",11);BackColor=Color.FromArgb(245,247,252);
        var root=new TableLayoutPanel{Dock=DockStyle.Fill,Padding=new Padding(24),ColumnCount=1,RowCount=11};
        foreach(int height in new[]{48,35,46,67,47,65,47,145})root.RowStyles.Add(new RowStyle(SizeType.Absolute,height));
        root.RowStyles.Add(new RowStyle(SizeType.Percent,100));root.RowStyles.Add(new RowStyle(SizeType.Absolute,48));root.RowStyles.Add(new RowStyle(SizeType.Absolute,30));Controls.Add(root);
        root.Controls.Add(new Label{Text=L.T("Votre salon PartyBoard"),Font=new Font("Segoe UI",23,FontStyle.Bold),AutoSize=true},0,0);
        root.Controls.Add(new Label{Text=L.T("Choisissez votre pseudo et votre disque. L'hôte lancera le jeu pour tous."),Dock=DockStyle.Fill},0,1);
        var identity=new FlowLayoutPanel{Dock=DockStyle.Fill};identity.Controls.Add(new Label{Text=L.T("Votre pseudo"),Width=116,Padding=new Padding(0,8,0,0)});
        nickname=new TextBox{Text="Joueur",Width=230,MaxLength=24,AccessibleName=L.T("Votre pseudo"),Margin=new Padding(0,5,12,0)};identity.Controls.Add(nickname);
        identity.Controls.Add(new Label{Text="Mode",AutoSize=true,Padding=new Padding(0,8,8,0),Margin=new Padding(36,0,0,0)});
        onlineMode=new RadioButton{Text=L.T("En ligne"),AutoSize=true,Checked=true,Margin=new Padding(0,8,12,0),AccessibleName=L.T("Mode en ligne")};
        lanMode=new RadioButton{Text=L.T("Réseau local (LAN)"),AutoSize=true,Margin=new Padding(0,8,12,0),AccessibleName=L.T("Mode réseau local")};
        onlineMode.CheckedChanged+=(s,e)=>ModeChanged();identity.Controls.Add(onlineMode);identity.Controls.Add(lanMode);
        root.Controls.Add(identity,0,2);
        var disk=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=2};disk.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,210));disk.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));
        choose=Make(L.T("Choisir mon disque"),ChooseDisc);disk.Controls.Add(choose,0,0);discLabel=new Label{Text=L.T("Mario Party 4 USA révision 1, ou européen\nISO, GCM ou RVZ — vérification du fichier complet"),Dock=DockStyle.Fill,Padding=new Padding(0,4,0,0)};disk.Controls.Add(discLabel,1,0);root.Controls.Add(disk,0,3);
        var top=new FlowLayoutPanel{Dock=DockStyle.Fill};
        top.Controls.Add(new Label{Text=L.T("Joueurs"),Padding=new Padding(0,8,4,0)});
        playerCount=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList,Width=64,Margin=new Padding(0,3,12,3),AccessibleName=L.T("Nombre de joueurs")};
        playerCount.Items.AddRange(new object[]{"2","3","4"});playerCount.SelectedIndex=0;
        playerCount.SelectedIndexChanged+=(s,e)=>{UpdateFooter();RefreshControls();};top.Controls.Add(playerCount);
        host=Make(L.T("Créer une room"),()=>Begin(true,"",SelectedPlayers()));top.Controls.Add(host);
        visibility=new ComboBox{DropDownStyle=ComboBoxStyle.DropDownList,Width=190,Margin=new Padding(0,6,12,3),AccessibleName=L.T("Visibilité de la room")};
        visibility.Items.AddRange(new object[]{L.T("Privée (amis invités)"),L.T("Publique")});visibility.SelectedIndex=0;
        visibility.SelectedIndexChanged+=(s,e)=>PublicChanged();top.Controls.Add(visibility);
        browse=Make(L.T("Rooms publiques…"),Browse);browse.MinimumSize=new Size(190,38);top.Controls.Add(browse);
        mods=Make("Mods…",ChooseMods);mods.MinimumSize=new Size(110,38);mods.AutoSizeMode=AutoSizeMode.GrowAndShrink;top.Controls.Add(mods);
        root.Controls.Add(top,0,4);
        invitation=new TextBox{Dock=DockStyle.Fill,Multiline=true,ScrollBars=ScrollBars.Vertical,Font=new Font("Segoe UI",10),AccessibleName=L.T("Invitation du salon"),MaxLength=220};root.Controls.Add(invitation,0,5);
        var actions=new FlowLayoutPanel{Dock=DockStyle.Fill};copy=Make(L.T("Copier le code"),()=>{Clipboard.SetText(invitation.Text);SetStatus(L.T("Code copié. Envoyez-le à votre ami ; gardez cette fenêtre ouverte."));});
        // Without CubeShelf, a friend's invitation still travels as a code.
        join=Make(L.T("Rejoindre avec un code"),()=>{if(session!=null)return;var text=invitation.Text.Trim();if(text.Length==0){text=(Clipboard.GetText()??"").Trim();invitation.Text=text;}if(text.Length==0){SetStatus(L.T("Collez le code d'invitation de votre ami, puis cliquez à nouveau."));return;}Begin(false,text);});
        paste=join;actions.Controls.Add(copy);actions.Controls.Add(join);root.Controls.Add(actions,0,6);
        var playersRow=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=2};
        // Absolute, not percent, so the mods panel takes a fixed slice and the
        // player list keeps essentially the width it always had -- a percent
        // split shrank every column and gave the list a horizontal scrollbar
        // even when the mods panel had nothing to show.
        playersRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));playersRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,170));
        players=new ListView{Dock=DockStyle.Fill,View=View.Details,FullRowSelect=true,HeaderStyle=ColumnHeaderStyle.Nonclickable,AccessibleName=L.T("Joueurs du salon"),HideSelection=false};
        players.Columns.Add(L.T("Pseudo"),165);players.Columns.Add(L.T("Rôle"),100);players.Columns.Add(L.T("Disque"),210);players.Columns.Add("Ping",80);playersRow.Controls.Add(players,0,0);
        missingMods=new FlowLayoutPanel{Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoScroll=true,Visible=false,Padding=new Padding(12,0,0,0),AccessibleName=L.T("Mods manquants")};
        playersRow.Controls.Add(missingMods,1,0);root.Controls.Add(playersRow,0,7);
        status=new Label{Text=L.T("Choisissez votre disque pour commencer."),Dock=DockStyle.Fill,Padding=new Padding(0,10,0,0),ForeColor=Color.FromArgb(35,49,72),AccessibleName=L.T("État du salon")};root.Controls.Add(status,0,8);
        var bottom=new FlowLayoutPanel{Dock=DockStyle.Fill};
        play=Make(L.T("Lancer pour tout le monde"),()=>{
            var current=session;if(current==null)return;play.Enabled=false;
            Task.Run(()=>{
                try{current.Launch();}
                catch(Exception e){UI(()=>{if(session==current){SetStatus(e.Message);RefreshLobby();}});}
            });
        });
        play.MinimumSize=new Size(240,38);
        cancel=Make(L.T("Quitter le salon"),Reset);bottom.Controls.Add(play);bottom.Controls.Add(cancel);root.Controls.Add(bottom,0,9);
        actions.Controls.Add(Make(L.T("Exporter diagnostic"),ExportReport));
        network=Make(L.T("Port fixe…"),ChooseNetwork);actions.Controls.Add(network);
        update=Make(L.T("Vérifier les mises à jour"),CheckForUpdates);actions.Controls.Add(update);
        footer=new Label{Dock=DockStyle.Fill,Font=new Font("Segoe UI",9),ForeColor=Color.DimGray};root.Controls.Add(footer,0,10);UpdateFooter();
        nickname.TextChanged+=(s,e)=>RefreshLobby();RefreshControls();RefreshLobby();
        // A launcher that shows the room itself offers updates itself: no dialog out of nowhere.
        if(!startup.Headless)Task.Run(()=>StartupUpdateCheck());else ShowInTaskbar=false;
        // Not in the constructor: UI() drops a callback while the handle does not exist, so a disc
        // that verified faster than the window appeared would vanish without a trace.
        Shown+=(s,e)=>{if(startupApplied)return;startupApplied=true;ApplyStartup();};
        // The launcher's view of this room (RoomSync), when it asked for one.
        if(startup.StateDirectory!=null) {
            var timer=new System.Windows.Forms.Timer{Interval=400};timer.Tick+=(s,e)=>SyncRoom();timer.Start();
            FormClosed+=(s,e)=>{timer.Stop();try{RoomSync.Write(startup.StateDirectory,new System.Collections.Generic.Dictionary<string,object>{{"state","closed"},{"message",status.Text}},++syncRevision);}catch{}};
        }
        FormClosing+=(s,e)=>{closing=true;fileCancel.Cancel();var old=session;session=null;if(old!=null)old.Dispose();if(disc!=null)disc.Dispose();};
    }
    Button network;
    // The fixed port (Gateway.cs, NetworkSettings): empty means automatic.
    void ChooseNetwork() {
        if(session!=null){SetStatus(L.T("Quittez le salon avant de changer le port."));return;}
        NetworkSettings.Load();
        using(var dialog=new Form{Text=L.T("Port fixe"),FormBorderStyle=FormBorderStyle.FixedDialog,StartPosition=FormStartPosition.CenterParent,MinimizeBox=false,MaximizeBox=false,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,Padding=new Padding(12),Font=Font}) {
            var layout=new TableLayoutPanel{ColumnCount=2,AutoSize=true,Dock=DockStyle.Fill};
            layout.Controls.Add(new Label{Text=L.T("Laissez vide pour la préparation automatique de la box.\nAvec un port fixe P, ouvrez sur votre box : TCP P, UDP P et UDP P+1."),AutoSize=true,MaximumSize=new Size(440,0),Margin=new Padding(0,0,0,10)},0,0);
            layout.SetColumnSpan(layout.GetControlFromPosition(0,0),2);
            layout.Controls.Add(new Label{Text=L.T("Port (1024 à 65534)"),AutoSize=true,Padding=new Padding(0,6,8,0)},0,1);
            var port=new TextBox{Text=NetworkSettings.Port==0?"":NetworkSettings.Port.ToString(),Width=120,AccessibleName=L.T("Port fixe")};layout.Controls.Add(port,1,1);
            layout.Controls.Add(new Label{Text=L.T("Adresse publique (facultatif)"),AutoSize=true,Padding=new Padding(0,6,8,0)},0,2);
            var address=new TextBox{Text=NetworkSettings.PublicAddress==null?"":NetworkSettings.PublicAddress.ToString(),Width=160,AccessibleName=L.T("Adresse publique")};layout.Controls.Add(address,1,2);
            layout.Controls.Add(new Label{Text=L.T("Seulement si la box ne la donne pas : celle affichée dans son interface."),AutoSize=true,MaximumSize=new Size(440,0),ForeColor=Color.DimGray,Margin=new Padding(0,4,0,10)},0,3);
            layout.SetColumnSpan(layout.GetControlFromPosition(0,3),2);
            var ok=new Button{Text=L.T("Enregistrer"),AutoSize=true,DialogResult=DialogResult.OK};var no=new Button{Text=L.T("Annuler"),AutoSize=true,DialogResult=DialogResult.Cancel};
            var buttons=new FlowLayoutPanel{AutoSize=true,FlowDirection=FlowDirection.RightToLeft,Dock=DockStyle.Fill};buttons.Controls.Add(no);buttons.Controls.Add(ok);
            layout.Controls.Add(buttons,0,4);layout.SetColumnSpan(buttons,2);
            dialog.Controls.Add(layout);dialog.AcceptButton=ok;dialog.CancelButton=no;
            if(dialog.ShowDialog(this)!=DialogResult.OK)return;
            int value=0;IPAddress publicAddress=null;
            if(port.Text.Trim().Length>0 && (!int.TryParse(port.Text.Trim(),out value) || value==0 || !NetworkSettings.Valid(value))){SetStatus(L.T("Port invalide : choisissez un nombre entre 1024 et 65534."));return;}
            if(address.Text.Trim().Length>0 && (!IPAddress.TryParse(address.Text.Trim(),out publicAddress) || !Gateway.Public(publicAddress))){SetStatus(L.T("Adresse publique invalide : indiquez une adresse IPv4 publique, par exemple 82.64.1.2."));return;}
            NetworkSettings.Port=value;NetworkSettings.PublicAddress=publicAddress;NetworkSettings.Save();
            SetStatus(value==0?L.T("Port automatique : la box sera préparée automatiquement."):L.T("Port fixe ")+value+L.T(" enregistré. Ouvrez sur votre box TCP ")+value+", UDP "+value+L.T(" et UDP ")+(value+1)+".");
        }
    }
    Button Make(string text,Action action){var b=new Button{Text=text,AutoSize=true,Height=38,MinimumSize=new Size(160,38),Margin=new Padding(0,3,12,3),FlatStyle=FlatStyle.Flat,BackColor=Color.White};b.Click+=(s,e)=>{try{action();}catch(Exception ex){SetStatus(Friendly(ex));}};return b;}
    int SelectedPlayers(){int n;return int.TryParse(playerCount.SelectedItem as string,out n)?n:2;}
    void UpdateFooter(){if(footer!=null)footer.Text=SelectedPlayers()+L.T(" joueurs · Même fichier disque requis · Ping : aller-retour entre les PC");}
    void UI(Action action){if(!closing && !IsDisposed){if(InvokeRequired){try{BeginInvoke(action);}catch(InvalidOperationException){}}else action();}}
    void SetStatus(string text){UI(()=>status.Text=text);}
    void RefreshControls(){bool idle=session==null;host.Enabled=join.Enabled=idle && disc!=null && !hashing;choose.Enabled=nickname.Enabled=idle && !hashing;playerCount.Enabled=idle;cancel.Enabled=!idle;invitation.ReadOnly=!idle;
        onlineMode.Enabled=lanMode.Enabled=idle;
        // LAN: no code at all, the list is the way in.
        invitation.Visible=copy.Visible=join.Visible=!Lan;visibility.Visible=!Lan;
        // The fixed port is for the box, which a LAN game never asks.
        if(network!=null)network.Visible=!Lan;
        // Two players: unchanged, Bridge appears the instant the one guest
        // connects. Above two, Bridge stays null for the whole session -- the
        // salon's own seat count is what says whether there is still room to
        // invite someone else.
        copy.Enabled=session!=null && session.Invite!=null && session.Host && (session.Lobby==null?session.Bridge==null:session.Lobby.Occupied<session.MaxPlayers);
        play.Enabled=session?.Lobby?.CanStart==true;
        mods.Enabled=!hashing && (idle || session.Lobby?.Phase==LobbyPhase.Waiting);
        browse.Enabled=idle && disc!=null && !hashing && (Lan || Relay.Available);
        // Before a room: a choice for the next one. During: the room itself,
        // for as long as it waits for its guest. Public needs the relay and
        // two players; anything else stays private.
        visibility.Enabled=Relay.Available && (idle?SelectedPlayers()==2:session.CanBePublic && session.Public!=PublicState.Pending);
        if(!idle && session.PublicNotices!=publicNoticesSeen){publicNoticesSeen=session.PublicNotices;SetPublicBox(session.Public==PublicState.Listed);}
        if(idle)publicNoticesSeen=0;
        if(idle && SelectedPlayers()!=2 && visibility.SelectedIndex==1)SetPublicBox(false);}
    void SetPublicBox(bool on){settingPublic=true;try{visibility.SelectedIndex=on?1:0;}finally{settingPublic=false;}}
    void ModeChanged(){
        if(session!=null)return;
        host.Text=Lan?L.T("Créer une partie LAN"):L.T("Créer une room");browse.Text=Lan?L.T("Parties en LAN…"):L.T("Rooms publiques…");
        Text=(Lan?L.T("PartyBoard — Réseau local v"):L.T("PartyBoard — Salon en ligne v"))+UpdateService.CurrentVersion;
        RefreshControls();
        if(disc!=null)SetStatus(Lan?L.T("Réseau local : créez une partie, ou trouvez celle d'un joueur de votre réseau dans « Parties en LAN ». Rien ne passe par Internet."):L.T("En ligne : créez une room privée pour vos amis ou une room publique, ou rejoignez une room publique."));
    }
    void PublicChanged(){
        if(settingPublic)return;var current=session;
        if(current==null){SetStatus(PublicWanted?L.T("La prochaine room sera publique : tous les joueurs qui ont le même disque la verront. Tes amis peuvent toujours être invités."):L.T("La prochaine room sera privée : seuls les amis que tu invites depuis CubeShelf pourront entrer."));return;}
        bool on=visibility.SelectedIndex==1;
        Task.Run(()=>{try{current.SetPublic(on);}catch(Exception e){UI(()=>{if(session==current){SetPublicBox(current.Public!=PublicState.Private);SetStatus(Friendly(e));}});}});
    }
    // A row of the list: who hosts, in which language, which disc, and how to get in.
    sealed class Listed {public string Name,Language,Region,Last,Key;public Func<string> Invite;}
    void Browse(){
        if(session!=null || disc==null || hashing)return;
        var build=Wire.BuildHash(AppDomain.CurrentDomain.BaseDirectory);var discHash=disc.Hash;
        if(Lan) BrowseList(L.T("Parties en LAN"),L.T("Parties sur le réseau local"),L.T("Joueurs"),()=>LanDiscovery.Find(build,discHash).Select(g=>new Listed{Name=g.Name,Language=g.Language,Region=g.Region,Last=g.Players+"/"+g.MaxPlayers,Key=g.Invite,Invite=()=>g.Invite}).ToList(),
            n=>n==0?L.T("Aucune partie sur votre réseau pour votre disque. Créez-en une, ou actualisez."):n+L.T(" partie")+(n>1?"s":"")+L.T(" sur votre réseau. Double-cliquez pour rejoindre."),L.T("Connexion à la partie sur votre réseau…"));
        else BrowseList(L.T("Rooms publiques"),L.T("Rooms publiques"),L.T("Ouverte depuis"),()=>Relay.PublicRooms(build,discHash).Select(r=>new Listed{Name=r.Name,Language=r.Language,Region=r.Region,Last=Since(r.Age),Key=r.Room,Invite=()=>Relay.PublicInvite(r.Room)}).ToList(),
            n=>n==0?L.T("Aucune room publique pour votre disque en ce moment. Créez-en une !"):n+" room"+(n>1?"s":"")+L.T(" pour votre disque. Double-cliquez pour rejoindre."),L.T("Connexion à la room publique…"));
    }
    // The games this player could join: the same disc and the same PartyBoard,
    // since anything else would be refused at the door.
    void BrowseList(string title,string listName,string lastColumn,Func<List<Listed>> source,Func<int,string> summary,string joining){
        if(session!=null || disc==null || hashing)return;
        using(var dialog=new Form{Text=title,ClientSize=new Size(640,420),StartPosition=FormStartPosition.CenterParent,Font=Font,MinimizeBox=false,MaximizeBox=false,FormBorderStyle=FormBorderStyle.FixedDialog}){
            var layout=new TableLayoutPanel{Dock=DockStyle.Fill,Padding=new Padding(16),RowCount=3,ColumnCount=1};
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute,30));layout.RowStyles.Add(new RowStyle(SizeType.Percent,100));layout.RowStyles.Add(new RowStyle(SizeType.Absolute,50));
            var note=new Label{Dock=DockStyle.Fill,Text=L.T("Chargement…")};layout.Controls.Add(note,0,0);
            var list=new ListView{Dock=DockStyle.Fill,View=View.Details,FullRowSelect=true,MultiSelect=false,HideSelection=false,AccessibleName=listName};
            list.Columns.Add(L.T("Hôte"),230);list.Columns.Add(L.T("Langue du jeu"),130);list.Columns.Add(L.T("Disque"),110);list.Columns.Add(lastColumn,120);layout.Controls.Add(list,0,1);
            var buttons=new FlowLayoutPanel{Dock=DockStyle.Fill,FlowDirection=FlowDirection.RightToLeft};
            var enter=new Button{Text=L.T("Rejoindre"),AutoSize=true,Enabled=false};var close=new Button{Text=L.T("Fermer"),AutoSize=true,DialogResult=DialogResult.Cancel};var refresh=new Button{Text=L.T("Actualiser"),AutoSize=true};
            buttons.Controls.Add(close);buttons.Controls.Add(enter);buttons.Controls.Add(refresh);layout.Controls.Add(buttons,0,2);
            dialog.Controls.Add(layout);dialog.CancelButton=close;
            Listed chosen=null;var rows=new List<Listed>();
            Action load=null;load=()=>{
                refresh.Enabled=false;note.Text=L.T("Chargement…");
                Task.Run(()=>{
                    try{
                        var found=source();
                        if(dialog.IsDisposed)return;
                        dialog.BeginInvoke((Action)(()=>{
                            list.BeginUpdate();list.Items.Clear();rows=found;
                            foreach(var r in found)list.Items.Add(new ListViewItem(new[]{r.Name,LanguageName(r.Language),r.Region=="eu"?L.T("Européen"):"USA",r.Last}){Tag=r});
                            list.EndUpdate();refresh.Enabled=true;
                            note.Text=summary(found.Count);
                            enter.Enabled=list.SelectedItems.Count>0;
                        }));
                    }catch(Exception e){if(!dialog.IsDisposed)try{dialog.BeginInvoke((Action)(()=>{note.Text=Friendly(e);refresh.Enabled=true;}));}catch(InvalidOperationException){}}
                });
            };
            Action pick=()=>{if(list.SelectedItems.Count==0)return;chosen=list.SelectedItems[0].Tag as Listed;dialog.DialogResult=DialogResult.OK;};
            refresh.Click+=(s,e)=>load();enter.Click+=(s,e)=>pick();list.DoubleClick+=(s,e)=>pick();
            list.SelectedIndexChanged+=(s,e)=>enter.Enabled=list.SelectedItems.Count>0;
            dialog.Shown+=(s,e)=>load();
            if(dialog.ShowDialog(this)!=DialogResult.OK || chosen==null)return;
            SetStatus(joining);
            Task.Run(()=>{
                try{var invite=chosen.Invite();UI(()=>{if(session==null){invitation.Text="";Begin(false,invite);}});}
                catch(Exception e){SetStatus(Friendly(e));}
            });
        }
    }
    static string LanguageName(string code){switch(code){case "fr":return "Français";case "de":return "Deutsch";case "es":return "Español";case "it":return "Italiano";default:return "English";}}
    static string Since(int seconds){return seconds<60?L.T("à l'instant"):seconds<3600?(seconds/60)+" min":(seconds/3600)+" h";}
    void RefreshLobby(){UI(()=>{
        if(players==null)return;RefreshControls();players.BeginUpdate();players.Items.Clear();
        var lobby=session?.Lobby;var local=lobby?.Local??session?.Profile;
        string localName=local?.Name??nickname.Text;
        bool localMatch=lobby!=null && lobby.SeatAgrees(lobby.LocalSeat);
        string localDisc=disc==null?L.T("À choisir"):localMatch?L.T("Identique — SHA-256 vérifié"):L.T("Vérifié — attente de comparaison");
        players.Items.Add(new ListViewItem(new[]{localName,session==null?L.T("Vous"):session.Host?L.T("Hôte (vous)"):L.T("Invité (vous)"),localDisc,"Local"}));
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
            string seatDisc=agrees?L.T("Identique — SHA-256 vérifié"):info.DiscHash==null?L.T("Non vérifié"):L.T("DISQUE DIFFÉRENT");
            // Per-seat ping only exists for the classic two-player Bridge; a
            // mesh session (session.Bridge stays null there) has no per-seat
            // latency measurement yet.
            string ping=session.Bridge==null?"—":session.Bridge.UdpReady && session.PingMs.HasValue?session.PingMs.Value+" ms":L.T("En attente…");
            players.Items.Add(new ListViewItem(new[]{info.Name,seat==0?L.T("Hôte"):L.T("Invité"),seatDisc,ping}));
        }
        players.EndUpdate();
        RefreshMissingMods(lobby);
        play.Text=session!=null && !session.Host?L.T("L'hôte lance la partie"):L.T("Lancer pour tout le monde");
        if(lobby==null)return;
        // A salon that has ended says so first. Until the peer could announce its
        // departure this branch had nothing to report, and the Running text below was
        // shown to someone whose partner had already quit.
        if(lobby.Ending==LobbyEnding.RemoteGameClosed)SetStatus(L.T("Votre ami a quitté la partie. Le salon est fermé — recréez-en un pour rejouer."));
        else if(lobby.Ending==LobbyEnding.LocalGameClosed)SetStatus(L.T("Votre partie est terminée. Le salon est fermé — recréez-en un pour rejouer."));
        else if(lobby.Phase==LobbyPhase.Preparing)SetStatus(L.T("Chargement sur les deux PC… Le jeu attendra que tout le monde soit prêt. Aucun bouton à presser dans l'autre fenêtre."));
        else if(lobby.Phase==LobbyPhase.Running)SetStatus(session.Bridge==null || session.Bridge.ControlConnected?L.T("Partie lancée par l'hôte. Gardez le salon ouvert pendant le jeu."):L.T("Le canal du salon est interrompu. La partie continue tant que l'autre joueur reste joignable. Gardez cette fenêtre ouverte."));
        else if(anyRemote) {
            if(!localMatch)SetStatus(L.T("Les fichiers disques sont différents : lancement bloqué. Quittez le salon, choisissez exactement le même fichier sur les deux PC, puis recréez le salon."));
            // Mods are checked after the disc because a different disc makes the mod
            // comparison meaningless, and reporting both at once helps nobody.
            // The salon fixes this itself now: a guest aligns in one click, the host
            // changes its own list with Mods…. Only a mod nobody here has installed
            // still means a trip to CubeShelf, and the salon stays open meanwhile.
            else if(!lobby.ModsMatch)SetStatus(session.Host
                ?L.T("Un invité n'a pas les mêmes mods que vous — ")+lobby.ModAdvice+L.T(". Il peut cliquer sur « Utiliser les mods de l'hôte », ou changez les vôtres avec « Mods… ».")
                :L.T("Vos mods ne correspondent pas à ceux de l'hôte — ")+lobby.ModAdvice+(lobby.Local.Mods.Missing(lobby.RequiredMods).Any()?L.T(". Installez les mods manquants dans CubeShelf, puis cliquez sur « Utiliser les mods de l'hôte »."):L.T(". Cliquez sur « Utiliser les mods de l'hôte ».")));
            else SetStatus(session.Host?L.T("Disques et mods identiques. Vous pouvez lancer la partie pour tout le monde."):L.T("Disques et mods identiques. Attendez que l'hôte lance la partie."));
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
        bool differs=lobby!=null && !session.Host && lobby.HostInfo!=null && lobby.Phase==LobbyPhase.Waiting && !lobby.Local.SameMods(lobby.HostInfo);
        missingMods.Visible=missing.Length>0 || differs;
        if(differs){
            var align=new Button{Text=L.T("Utiliser les mods de l'hôte"),AutoSize=true,MaximumSize=new Size(160,0),FlatStyle=FlatStyle.Flat,BackColor=Color.White,Margin=new Padding(0,0,0,10)};
            align.Click+=(s,e)=>{try{AlignOnHost();}catch(Exception ex){SetStatus(Friendly(ex));}};
            missingMods.Controls.Add(align);
        }
        if(missing.Length>0){
            missingMods.Controls.Add(new Label{Text=L.T("Mods manquants"),Font=new Font("Segoe UI",10,FontStyle.Bold),AutoSize=true,Margin=new Padding(0,0,0,6)});
            foreach(var mod in missing){
                var entry=mod;
                var row=new FlowLayoutPanel{FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoSize=true,Margin=new Padding(0,0,0,10)};
                row.Controls.Add(new Label{Text=entry.Describe(),AutoSize=true,MaximumSize=new Size(160,0)});
                var download=new Button{Text=L.T("Télécharger"),AutoSize=true,Height=30,FlatStyle=FlatStyle.Flat,BackColor=Color.White,Margin=new Padding(0,3,0,0)};
                download.Click+=(s,e)=>{try{Process.Start(new ProcessStartInfo(entry.GameBananaUrl()){UseShellExecute=true});SetStatus(L.T("Page GameBanana de ")+entry.Describe()+L.T(" ouverte dans votre navigateur."));}catch(Exception ex){SetStatus(Friendly(ex));}};
                row.Controls.Add(download);
                missingMods.Controls.Add(row);
            }
        }
        missingMods.ResumeLayout();
    }
    void ChooseDisc(){
        if(session!=null || hashing)return;
        using(var picker=new OpenFileDialog{Title=L.T("Choisir Mario Party 4 (USA Rev 1 ou européen)"),Filter=L.T("Images de disque|*.iso;*.gcm;*.rvz;*.wia;*.gcz;*.ciso|Tous les fichiers|*.*"),CheckFileExists=true}){
            if(picker.ShowDialog(this)!=DialogResult.OK)return;
            LoadDisc(picker.FileName,pendingStartupAction?(Action)RunStartupAction:null);
        }
    }
    // The same verification whether the player picked the file or a launcher named it. onVerified
    // runs on the UI thread once the hash is complete, and never when it failed.
    void LoadDisc(string path,Action onVerified){
        if(session!=null || hashing || string.IsNullOrEmpty(path))return;
        if(disc!=null){disc.Dispose();disc=null;}hashing=true;RefreshLobby();SetStatus(L.T("Vérification du disque… Vous pouvez laisser cette fenêtre ouverte."));
        Task.Run(()=>{try{
            var verified=DiscFile.Verify(path,p=>UI(()=>discLabel.Text=Path.GetFileName(path)+L.T("\nVérification SHA-256 : ")+p+" %"),fileCancel.Token);
            if(closing){verified.Dispose();return;}
            UI(()=>{disc=verified;hashing=false;discLabel.Text=Path.GetFileName(path)+L.T("\nFichier vérifié et protégé contre les modifications");SetStatus(Lan?L.T("Disque vérifié. Créez une partie LAN ou trouvez-en une dans « Parties en LAN »."):L.T("Disque vérifié. Créez une room, ou rejoignez une room publique ou l'invitation d'un ami."));RefreshLobby();if(onVerified!=null)onVerified();});
        }catch(Exception e){UI(()=>{hashing=false;discLabel.Text=L.T("Aucun disque vérifié");SetStatus(Friendly(e));RefreshLobby();});}});
    }
    void Begin(bool create,string invitationText,int players=2){
        if(session!=null || disc==null || hashing)return;
        // The opening list: what was picked in Mods…, else what CubeShelf would load.
        // It can change afterwards only through Session.ChangeMods, which re-announces
        // it and is refused once a launch begins, so nothing the peers did not see can
        // reach the game.
        string modsFrom;ModSet opening;
        try {opening=chosenMods??LauncherMods()??ModSet.FromCubeShelf(GameId,out modsFrom);}
        catch(Exception e){SetStatus(Friendly(e));return;}
        var profile=new PlayerInfo(nickname.Text,disc.Hash,disc.Length,opening,GameLanguage.Current);
        Session current=null;current=new Session(t=>UI(()=>{if(session==current)SetStatus(t);}),()=>UI(()=>{if(session==current)RefreshLobby();}),t=>UI(()=>{if(session==current){Reset();SetStatus(t);}}),profile,disc);
        session=current;lastReport=current.Report;current.Host=create;current.Lan=Lan;if(create)current.RoomName=startup.RoomName;RefreshLobby();
        current.Report.Write("role="+(create?"host":"guest")+" connection_requested");
        Task.Run(()=>{try{
            if(create){current.Create(players);UI(()=>{if(session==current && current.Bridge==null){var encoded=current.Invite.Encode();invitation.Text=current.Lan?"":encoded;if(!current.Lan)PublishInvitation(encoded);RefreshControls();
                if(PublicWanted){if(current.CanBePublic)Task.Run(()=>{try{current.SetPublic(true);}catch(Exception e){UI(()=>{if(session==current){SetPublicBox(false);SetStatus(Friendly(e));}});}});else{SetPublicBox(false);SetStatus(L.T("La room ne peut pas être publique : le relais en ligne n'est pas disponible. Elle reste privée."));return;}}SetStatus(CreatedMessage(current,players));}});}
            else current.Join(invitationText);
        }catch(Exception e){
            var socket=e as System.Net.Sockets.SocketException;
            current.Report.Write("event=start_failed error="+e.GetType().Name+(socket!=null?" socket="+socket.SocketErrorCode:"")+" at="+(e.TargetSite!=null?e.TargetSite.Name:"?"));
            current.Dispose();UI(()=>{if(session==current){Reset();SetStatus(Friendly(e));}});}});
    }
    // What the host reads once its room or LAN game exists: who can come in, and how.
    string CreatedMessage(Session current,int players){
        if(current.Lan)return L.T("Partie LAN créée. Les joueurs de votre réseau la trouvent dans « Parties en LAN » ; rien ne passe par Internet. Gardez cette fenêtre ouverte.");
        bool launcher=startup.InvitationOut!=null;
        if(current.LocalOnly && !current.RelayReady)return L.T("La room n'est pas joignable depuis Internet : votre box refuse et le relais en ligne ne répond pas. Pour jouer à la maison, choisissez « Réseau local (LAN) » ; sinon, réessayez plus tard ou ouvrez un port fixe.");
        string via=current.LocalOnly?L.T(" Votre box refuse les connexions depuis Internet : tout passera par le relais en ligne."):"";
        if(PublicWanted)return L.T("Room créée, publication en cours…")+via;
        return launcher?L.T("Room privée créée. Invitez vos amis depuis la liste d'amis de CubeShelf ; gardez cette fenêtre ouverte.")+via
            :L.T("Room privée créée. Invitez vos amis depuis CubeShelf, ou copiez le code et envoyez-le")+(players>2?L.T(" à vos amis."):L.T(" à votre ami."))+via;
    }
    const string GameId="GMPE01_00";
    // The mods a launcher asked this session to play with (PARTYBOARD_ONLINE_MODS),
    // rebuilt from what is installed here: a room's list, joined without touching
    // the launcher's own settings. Null when it asked for nothing.
    ModSet LauncherMods(){
        if(startup.ModIds==null)return null;
        string from;var installed=InstalledMod.FromCubeShelf(GameId,out from);
        var wanted=new ModSet(startup.ModIds.Select(id=>new ModEntry(id,"","",new byte[8])));
        return ModSet.AlignedTo(wanted,installed);
    }
    ModSet CurrentMods(){var lobby=session?.Lobby;if(lobby!=null)return lobby.Local.Mods;if(session!=null)return session.Profile.Mods;string from;return chosenMods??ModSet.FromCubeShelf(GameId,out from);}
    // The same window whether CubeShelf opened the salon or PartyBoard did: both read
    // CubeShelf's installed mods (PARTYBOARD_MOD_LIST says where, when it is set) and
    // neither writes anything back. A choice made here lasts for this salon only.
    void ChooseMods(){
        string from;var installed=InstalledMod.FromCubeShelf(GameId,out from);
        if(installed.Count==0){SetStatus(L.T("Aucun mod installé dans CubeShelf pour Mario Party 4. Installez-en depuis CubeShelf, puis rouvrez « Mods… »."));return;}
        var current=CurrentMods();var lobby=session?.Lobby;
        using(var dialog=new Form{Text=L.T("Mods de ce salon"),FormBorderStyle=FormBorderStyle.FixedDialog,StartPosition=FormStartPosition.CenterParent,MinimizeBox=false,MaximizeBox=false,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,Padding=new Padding(12),Font=Font}) {
            var layout=new TableLayoutPanel{ColumnCount=1,AutoSize=true,Dock=DockStyle.Fill};
            layout.Controls.Add(new Label{Text=L.T("Cochez les mods à utiliser dans ce salon. Tous les joueurs doivent avoir les mêmes, dans le même ordre. CubeShelf n'est pas modifié."),AutoSize=true,MaximumSize=new Size(460,0),Margin=new Padding(0,0,0,8)});
            var list=new CheckedListBox{Width=460,Height=Math.Min(12,Math.Max(4,installed.Count))*24+8,CheckOnClick=true,IntegralHeight=false,AccessibleName=L.T("Mods installés")};
            foreach(var mod in installed)list.Items.Add(mod.Entry.Describe(),current.Find(mod.Entry.Id)!=null);
            layout.Controls.Add(list);
            var buttons=new FlowLayoutPanel{AutoSize=true,FlowDirection=FlowDirection.RightToLeft,Dock=DockStyle.Fill,Margin=new Padding(0,10,0,0)};
            var ok=new Button{Text=L.T("Appliquer"),AutoSize=true,DialogResult=DialogResult.OK};var no=new Button{Text=L.T("Annuler"),AutoSize=true,DialogResult=DialogResult.Cancel};
            buttons.Controls.Add(no);buttons.Controls.Add(ok);
            var reset=new Button{Text=L.T("Comme CubeShelf"),AutoSize=true};reset.Click+=(s,e)=>{for(int i=0;i<installed.Count;i++)list.SetItemChecked(i,installed[i].On);};buttons.Controls.Add(reset);
            if(lobby!=null && !session.Host && lobby.RequiredMods!=null){
                var host=new Button{Text=L.T("Comme l'hôte"),AutoSize=true};
                host.Click+=(s,e)=>{for(int i=0;i<installed.Count;i++)list.SetItemChecked(i,lobby.RequiredMods.Find(installed[i].Entry.Id)!=null);};
                buttons.Controls.Add(host);
            }
            layout.Controls.Add(buttons);dialog.Controls.Add(layout);dialog.AcceptButton=ok;dialog.CancelButton=no;
            if(dialog.ShowDialog(this)!=DialogResult.OK)return;
            var ticked=new System.Collections.Generic.HashSet<int>();for(int i=0;i<installed.Count;i++)if(list.GetItemChecked(i))ticked.Add(installed[i].Entry.Id);
            // A guest who ticked exactly the host's mods means the host's order too:
            // CubeShelf's priorities on this PC are not what the session agreed on.
            var required=lobby!=null && !session.Host?lobby.RequiredMods:null;
            ModSet picked=required!=null && required.Entries.Length==ticked.Count && required.Entries.All(m=>ticked.Contains(m.Id))
                ?ModSet.AlignedTo(required,installed)
                :ModSet.Pick(installed,e=>ticked.Contains(e.Id),current);
            ApplyMods(picked);
        }
    }
    void AlignOnHost(){
        var lobby=session?.Lobby;if(lobby==null || session.Host || lobby.RequiredMods==null)return;
        string from;ApplyMods(ModSet.AlignedTo(lobby.RequiredMods,InstalledMod.FromCubeShelf(GameId,out from)));
    }
    void ApplyMods(ModSet picked){
        var current=session;
        if(current==null){chosenMods=picked;SetStatus(picked.None?L.T("Aucun mod pour ce salon."):L.T("Mods de ce salon : ")+string.Join(", ",picked.Entries.Select(m=>m.Describe()).ToArray())+".");return;}
        mods.Enabled=false;
        Task.Run(()=>{
            try{current.ChangeMods(picked);}
            catch(Exception e){UI(()=>{if(session==current)SetStatus(Friendly(e));});}
            UI(()=>{if(session==current)RefreshLobby();});
        });
    }
    internal void PreviewLobby(){
        // Deterministic render fixture; no session, network, or selected disk.
        players.Items.Clear();players.Items.Add(new ListViewItem(new[]{"Camille",L.T("Hôte (vous)"),L.T("Identique — SHA-256 vérifié"),"Local"}));players.Items.Add(new ListViewItem(new[]{"Alex",L.T("Invité"),L.T("Identique — SHA-256 vérifié"),"42 ms"}));
        nickname.Text="Camille";players.Items.Clear();players.Items.Add(new ListViewItem(new[]{"Camille",L.T("Hôte (vous)"),L.T("Identique — SHA-256 vérifié"),"Local"}));players.Items.Add(new ListViewItem(new[]{"Alex",L.T("Invité"),L.T("Identique — SHA-256 vérifié"),"42 ms"}));
        discLabel.Text=L.T("Mario Party 4.iso\nFichier vérifié");play.Enabled=true;SetStatus(L.T("Aperçu de l'interface — joueurs et ping fictifs. Seul l'hôte peut lancer."));
    }
    void ExportReport(){
        string content=lastReport!=null?lastReport.Read():Report.ReadLatest();
        using(var save=new SaveFileDialog{Title=L.T("Enregistrer le diagnostic"),Filter=L.T("Diagnostic texte|*.txt"),FileName="Diagnostic-PartyBoard.txt",DefaultExt="txt",AddExtension=true}){
            if(save.ShowDialog(this)!=DialogResult.OK)return;
            File.WriteAllText(save.FileName,content,System.Text.Encoding.UTF8);
            SetStatus(L.T("Diagnostic enregistré. Envoyez ce fichier et celui de l'autre PC dans la conversation."));
        }
    }
    async void CheckForUpdates(){
        if(session!=null){SetStatus(L.T("Quittez le salon avant de mettre à jour PartyBoard."));return;}
        update.Enabled=false;SetStatus(L.T("Recherche d'une mise à jour sur GitHub…"));
        try{
            var info=await UpdateService.CheckAsync();
            var current=new Version(UpdateService.CurrentVersion);
            if(info.Version<=current){SetStatus(L.T("PartyBoard est déjà à jour (v")+UpdateService.CurrentVersion+").");return;}
            var notes=String.IsNullOrWhiteSpace(info.Notes)?L.T("Une nouvelle version est disponible."):info.Notes;
            if(MessageBox.Show(this,L.T("La version v")+info.Version+" est disponible.\n\n"+notes+L.T("\n\nTélécharger et installer maintenant ?"),L.T("Mise à jour PartyBoard"),MessageBoxButtons.YesNo,MessageBoxIcon.Information)!=DialogResult.Yes){SetStatus(L.T("Mise à jour reportée."));return;}
            SetStatus(L.T("Téléchargement de la mise à jour…"));await UpdateService.InstallAsync(info);
            MessageBox.Show(this,L.T("La mise à jour a été téléchargée. PartyBoard va redémarrer pour l'installer."),L.T("Mise à jour PartyBoard"),MessageBoxButtons.OK,MessageBoxIcon.Information);
            closing=true;Application.Exit();
        }catch(Exception e){SetStatus(L.T("Impossible de vérifier la mise à jour : ")+e.Message);}
        finally{if(!closing && !IsDisposed)update.Enabled=true;}
    }
    async Task StartupUpdateCheck(){
        try{
            var info=await UpdateService.CheckAsync();
            if(info.Version<=new Version(UpdateService.CurrentVersion))return;
            UI(()=>{if(update!=null && !IsDisposed){update.Text=L.T("Mise à jour disponible");update.Enabled=true;SetStatus(L.T("Une mise à jour est disponible sur GitHub. Cliquez sur le bouton de mise à jour pour l'installer."));}});
        }catch{
            // The launcher remains fully usable when GitHub is offline or blocked.
        }
    }
    static string Friendly(Exception e){if(e is IOException)return e.Message;
        // A SocketException is a Win32Exception too, but it is the network, not a refusal.
        if(e is System.Net.Sockets.SocketException)return L.T("Le réseau a refusé l'opération (")+((System.Net.Sockets.SocketException)e).SocketErrorCode+L.T("). Vérifiez votre connexion, puis réessayez.");if(e is System.ComponentModel.Win32Exception)return L.T("Windows n'a pas donné son autorisation. Réessayez et acceptez sa demande.");if(e is OperationCanceledException)return L.T("Vérification annulée.");return L.T("L'opération n'a pas abouti. Vérifiez votre connexion ou recréez le salon.");}
    // What CubeShelf (or any launcher) asked for before the player touched anything.
    //
    // Nothing here bypasses a rule the player would otherwise meet: the disc is verified with the
    // same full SHA-256, the lobby refuses mismatched files just the same, and only the host can
    // start the game. It removes the typing, not the checks.
    void ApplyStartup(){
        if(startup.Mode==StartupMode.Manual)return;
        // A launcher's rooms are online: private for friends unless it asked
        // for a public one. LAN games are this window's own.
        onlineMode.Checked=true;
        if(startup.Mode==StartupMode.Host && startup.Public)SetPublicBox(true);
        if(startup.Nickname!=null)nickname.Text=startup.Nickname;
        if(startup.Invitation!=null)invitation.Text=startup.Invitation;

        // Without a disc there is nothing to verify and nothing to join with. Say which step is
        // missing rather than opening a window that looks ready and refuses on click.
        if(startup.DiscPath==null || !File.Exists(startup.DiscPath)){
            SetStatus(startup.Mode==StartupMode.Join
                ?L.T("Invitation reçue. Choisissez votre disque : la partie commencera dès qu'il sera vérifié.")
                :L.T("Choisissez votre disque : le salon sera créé dès qu'il sera vérifié."));
            pendingStartupAction=true;RefreshLobby();return;
        }
        LoadDisc(startup.DiscPath,RunStartupAction);
    }
    bool pendingStartupAction;
    // Headless (PARTYBOARD_ONLINE_HEADLESS, with a state directory): the launcher shows the
    // room, so this window never appears; everything else runs as it would, room.json carries
    // what it would have shown and the launcher's requests do what its buttons would have.
    protected override void SetVisibleCore(bool value) {
        if(!startup.Headless){base.SetVisibleCore(value);return;}
        if(!IsHandleCreated){CreateHandle();BeginInvoke((Action)(()=>{if(startupApplied)return;startupApplied=true;ApplyStartup();}));}
        base.SetVisibleCore(false);
    }
    bool hadSession;
    long syncRevision;string syncLast;
    // What room.json says: this window's room, in the launcher's words.
    System.Collections.Generic.Dictionary<string,object> RoomState() {
        var current=session;
        var state=new System.Collections.Generic.Dictionary<string,object>{{"mode",Lan?"lan":"online"},{"visibility",PublicWanted?"public":"private"},{"message",status.Text}};
        if(current==null){state["state"]=disc==null?"needs-disc":"idle";return state;}
        state["role"]=current.Host?"host":"guest";
        if(current.Host && current.Invite==null){state["state"]="creating";return state;}
        if(!current.Host && current.Lobby==null){state["state"]="joining";return state;}
        var room=current.RoomStatus();
        state["state"]=room.State;state["game"]=room.Game;state["players"]=room.Players.ToArray();state["max"]=room.MaxPlayers;
        state["mods"]=room.Fields()["mods"];
        state["public"]=current.Public==PublicState.Listed;
        // Which relay room this is, host or guest, so the launcher never lists it as someone else's.
        if(current.Invite!=null && !current.Lan)state["roomId"]=Relay.RoomId(current.Invite.Token);
        if(current.PingMs.HasValue)state["ping"]=current.PingMs.Value;
        if(current.Lan)state["path"]="lan";else if(current.Bridge!=null)state["path"]=current.JoinPath;
        var lobby=current.Lobby;
        if(lobby!=null) {
            // Every seat as this window's list shows it: who, which role, whether the disc
            // matched, the mods, and the ping (two players only).
            var seats=new System.Collections.Generic.List<object>();
            for(int seat=0;seat<Lobby.MaxSeats;seat++) {
                var info=lobby.SeatInfo(seat);if(info==null)continue;
                bool mine=seat==lobby.LocalSeat;bool agrees=lobby.SeatAgrees(seat);
                seats.Add(new System.Collections.Generic.Dictionary<string,object>{
                    {"name",info.Name},{"host",seat==0},{"you",mine},
                    {"disc",agrees?"same":info.DiscHash==null?"unknown":mine?"pending":"different"},
                    {"mods",new Relay.RoomStatus{Mods=info.Mods}.Fields()["mods"]},
                    {"ping",!mine && current.Bridge!=null && current.Bridge.UdpReady && current.PingMs.HasValue?(object)current.PingMs.Value:null}});
            }
            state["seats"]=seats.ToArray();
            state["phase"]=lobby.Phase.ToString().ToLowerInvariant();
            state["canStart"]=lobby.CanStart;
            state["discsMatch"]=lobby.DiscMatches;
            state["modsMatch"]=lobby.ModsMatch;
            state["modAdvice"]=lobby.ModAdvice;
            if(lobby.Ending!=LobbyEnding.None)state["ending"]=lobby.Ending.ToString();
            if(!current.Host && lobby.RequiredMods!=null) {
                state["missingMods"]=new Relay.RoomStatus{Mods=new ModSet(lobby.Local.Mods.Missing(lobby.RequiredMods))}.Fields()["mods"];
                state["differsFromHost"]=lobby.HostInfo!=null && !lobby.Local.SameMods(lobby.HostInfo);
            }
        }
        // The invitation goes to the launcher for friends only: a private,
        // online room it hosts and that still has a seat.
        if(current.Host && !current.Lan && room.State=="waiting")state["invite"]=current.Invite.Encode();
        return state;
    }
    void SyncRoom() {
        if(startup.StateDirectory==null || closing)return;
        try {
            // Headless, a room that ended (left, failed, or its game is over) has nothing left
            // to show here: the launcher is told why, and this process goes.
            if(session!=null)hadSession=true;
            if(startup.Headless && hadSession && session==null) {
                RoomSync.Write(startup.StateDirectory,new System.Collections.Generic.Dictionary<string,object>{{"state","closed"},{"message",status.Text}},++syncRevision);
                Close();return;
            }
            var state=RoomState();
            var key=new System.Web.Script.Serialization.JavaScriptSerializer().Serialize(state);
            if(key!=syncLast){RoomSync.Write(startup.StateDirectory,state,++syncRevision);syncLast=key;}
            foreach(var request in RoomSync.Read(startup.StateDirectory,DateTime.UtcNow))Act(request);
        } catch(IOException) {} catch(UnauthorizedAccessException) {}
    }
    void Act(RoomSync.Request request) {
        var current=session;
        switch(request.Action) {
            case "set-visibility": {
                bool on=request.Value=="public";
                if(on && (Lan || SelectedPlayers()!=2 && current==null)){RoomSync.Complete(startup.StateDirectory,request,false,L.T("Seule une room en ligne à 2 joueurs peut être publique."));return;}
                if(current!=null && on && !current.RelayReady){RoomSync.Complete(startup.StateDirectory,request,false,L.T("Le relais en ligne n'est pas disponible : la room reste privée."));return;}
                if(current!=null && !current.CanBePublic){RoomSync.Complete(startup.StateDirectory,request,false,L.T("Cette room ne peut plus changer de visibilité."));return;}
                if(visibility.SelectedIndex!=(on?1:0))visibility.SelectedIndex=on?1:0; // PublicChanged does the rest
                RoomSync.Complete(startup.StateDirectory,request,true,on?L.T("Room publique."):L.T("Room privée."));return;
            }
            case "reload-mods": {
                chosenMods=null;
                if(current==null){RoomSync.Complete(startup.StateDirectory,request,true,L.T("Les mods seront pris au lancement de la room."));return;}
                try {
                    string from;current.ChangeMods(ModSet.FromCubeShelf(GameId,out from));RefreshLobby();
                    RoomSync.Complete(startup.StateDirectory,request,true,L.T("Mods mis à jour dans la room."));
                } catch(Exception e) {RoomSync.Complete(startup.StateDirectory,request,false,Friendly(e));}
                return;
            }
            case "close":
                RoomSync.Complete(startup.StateDirectory,request,true,L.T("Room fermée."));
                hadSession=false;Reset();Close();return;
            case "start": {
                if(current==null || current.Lobby==null || !current.Lobby.CanStart){RoomSync.Complete(startup.StateDirectory,request,false,current!=null && !current.Host?L.T("Seul l'hote peut lancer la partie."):L.T("La partie ne peut pas encore être lancée."));return;}
                Task.Run(()=>{
                    try{current.Launch();RoomSync.Complete(startup.StateDirectory,request,true,L.T("Partie lancée."));}
                    catch(Exception e){RoomSync.Complete(startup.StateDirectory,request,false,e.Message);UI(()=>{if(session==current){SetStatus(e.Message);RefreshLobby();}});}
                });
                return;
            }
            case "align-host": {
                if(current==null || current.Host || current.Lobby?.RequiredMods==null){RoomSync.Complete(startup.StateDirectory,request,false,L.T("Attendez que votre ami soit connecté."));return;}
                AlignOnHost();RoomSync.Complete(startup.StateDirectory,request,true,L.T("Mods mis à jour dans la room."));return;
            }
            case "set-mods": {
                var ids=Startup.ParseMods(request.Value??"");
                if(ids==null){RoomSync.Complete(startup.StateDirectory,request,false,L.T("Liste de mods trop longue."));return;}
                string from;var installed=InstalledMod.FromCubeShelf(GameId,out from);
                ApplyMods(ModSet.AlignedTo(new ModSet(ids.Select(id=>new ModEntry(id,"","",new byte[8]))),installed));
                RoomSync.Complete(startup.StateDirectory,request,true,L.T("Mods mis à jour dans la room."));return;
            }
        }
    }
    void RunStartupAction(){
        if(startup.Mode==StartupMode.Manual || session!=null)return;
        pendingStartupAction=false;
        if(startup.Mode==StartupMode.Host)Begin(true,"",SelectedPlayers());
        else Begin(false,invitation.Text);
    }
    // Writing the invitation where the launcher is watching is what lets it show "invite your
    // friends" without the host copying anything by hand. Best effort: a lobby that was created
    // must not be torn down because a file could not be written.
    void PublishInvitation(string encoded){
        if(startup.InvitationOut==null || string.IsNullOrEmpty(encoded))return;
        try{
            var directory=Path.GetDirectoryName(startup.InvitationOut);
            if(!string.IsNullOrEmpty(directory))Directory.CreateDirectory(directory);
            var temporary=startup.InvitationOut+".tmp";
            File.WriteAllText(temporary,encoded,new System.Text.UTF8Encoding(false));
            if(File.Exists(startup.InvitationOut))File.Delete(startup.InvitationOut);
            File.Move(temporary,startup.InvitationOut);
        }catch(Exception e){
            if(!(e is IOException || e is UnauthorizedAccessException || e is NotSupportedException || e is ArgumentException))throw;
        }
    }
    void Reset(){var old=session;session=null;if(old!=null)Task.Run(()=>old.Dispose());RefreshLobby();SetStatus(disc!=null?(Lan?L.T("Créez une partie LAN ou trouvez-en une dans « Parties en LAN »."):L.T("Créez une room, ou rejoignez une room publique ou l'invitation d'un ami.")):L.T("Choisissez votre disque pour commencer."));}
}
}
