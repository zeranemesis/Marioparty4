using System;
using System.IO;
using System.Drawing;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PartyBoardOnline {
sealed class MainForm : Form {
    Label status,discLabel;TextBox invitation,nickname;ListView players;
    Button host,join,copy,play,cancel,choose,paste;Session session;DiscFile disc;
    readonly CancellationTokenSource fileCancel=new CancellationTokenSource();bool closing,hashing;
    Report lastReport;
    public MainForm() {
        Text="PartyBoard — Salon en ligne v6.3";ClientSize=new Size(840,760);MinimumSize=new Size(800,790);
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
        var top=new FlowLayoutPanel{Dock=DockStyle.Fill};host=Make("Créer un salon",()=>Begin(true,""));join=Make("Rejoindre",()=>Begin(false,invitation.Text));top.Controls.Add(host);top.Controls.Add(join);root.Controls.Add(top,0,4);
        invitation=new TextBox{Dock=DockStyle.Fill,Multiline=true,ScrollBars=ScrollBars.Vertical,Font=new Font("Segoe UI",10),AccessibleName="Invitation du salon",MaxLength=220};root.Controls.Add(invitation,0,5);
        var actions=new FlowLayoutPanel{Dock=DockStyle.Fill};copy=Make("Copier l'invitation",()=>{Clipboard.SetText(invitation.Text);SetStatus("Invitation copiée. Envoyez-la à votre ami ; gardez cette fenêtre ouverte.");});
        paste=Make("Coller l'invitation",()=>{if(session==null)invitation.Text=Clipboard.GetText();});actions.Controls.Add(copy);actions.Controls.Add(paste);root.Controls.Add(actions,0,6);
        players=new ListView{Dock=DockStyle.Fill,View=View.Details,FullRowSelect=true,HeaderStyle=ColumnHeaderStyle.Nonclickable,AccessibleName="Joueurs du salon",HideSelection=false};
        players.Columns.Add("Pseudo",215);players.Columns.Add("Rôle",105);players.Columns.Add("Disque",290);players.Columns.Add("Ping",110);root.Controls.Add(players,0,7);
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
        root.Controls.Add(new Label{Text="2 joueurs · Même fichier disque requis · Ping : aller-retour vers l'autre PC",Dock=DockStyle.Fill,Font=new Font("Segoe UI",9),ForeColor=Color.DimGray},0,10);
        nickname.TextChanged+=(s,e)=>RefreshLobby();RefreshControls();RefreshLobby();
        FormClosing+=(s,e)=>{closing=true;fileCancel.Cancel();var old=session;session=null;if(old!=null)old.Dispose();if(disc!=null)disc.Dispose();};
    }
    Button Make(string text,Action action){var b=new Button{Text=text,AutoSize=true,Height=38,MinimumSize=new Size(160,38),Margin=new Padding(0,3,12,3),FlatStyle=FlatStyle.Flat,BackColor=Color.White};b.Click+=(s,e)=>{try{action();}catch(Exception ex){SetStatus(Friendly(ex));}};return b;}
    void UI(Action action){if(!closing && !IsDisposed){if(InvokeRequired){try{BeginInvoke(action);}catch(InvalidOperationException){}}else action();}}
    void SetStatus(string text){UI(()=>status.Text=text);}
    void RefreshControls(){bool idle=session==null;host.Enabled=join.Enabled=idle && disc!=null && !hashing;choose.Enabled=nickname.Enabled=idle && !hashing;paste.Enabled=idle;cancel.Enabled=!idle;invitation.ReadOnly=!idle;copy.Enabled=session!=null && session.Invite!=null && session.Host && session.Bridge==null;play.Enabled=session?.Lobby?.CanStart==true;}
    void RefreshLobby(){UI(()=>{
        if(players==null)return;RefreshControls();players.BeginUpdate();players.Items.Clear();
        var lobby=session?.Lobby;var local=lobby?.Local??session?.Profile;
        string localName=local?.Name??nickname.Text;bool match=lobby?.DiscMatches==true;
        string localDisc=disc==null?"À choisir":match?"Identique — SHA-256 vérifié":"Vérifié — attente de comparaison";
        players.Items.Add(new ListViewItem(new[]{localName,session==null?"Vous":session.Host?"Hôte (vous)":"Invité (vous)",localDisc,"Local"}));
        if(lobby?.Remote!=null){var remote=lobby.Remote;players.Items.Add(new ListViewItem(new[]{remote.Name,session.Host?"Invité":"Hôte",match?"Identique — SHA-256 vérifié":remote.DiscHash==null?"Non vérifié":"DISQUE DIFFÉRENT",session.Bridge?.UdpReady==true && session.PingMs.HasValue?session.PingMs.Value+" ms":"En attente…"}));}
        players.EndUpdate();
        play.Text=session!=null && !session.Host?"L'hôte lance la partie":"Lancer pour tout le monde";
        if(lobby==null)return;
        if(lobby.Phase==LobbyPhase.Preparing)SetStatus("Chargement sur les deux PC… Le jeu attendra que tout le monde soit prêt. Aucun bouton à presser dans l'autre fenêtre.");
        else if(lobby.Phase==LobbyPhase.Running)SetStatus(session.Bridge.ControlConnected?"Partie lancée par l'hôte. Gardez le salon ouvert pendant le jeu.":"Le canal du salon est interrompu. La partie continue tant que l'autre joueur reste joignable. Gardez cette fenêtre ouverte.");
        else if(lobby.Remote!=null)SetStatus(match?(session.Host?"Les disques sont identiques. Vous pouvez lancer la partie pour tout le monde.":"Disques identiques. Attendez que l'hôte lance la partie."):"Les fichiers disques sont différents : lancement bloqué. Quittez le salon, choisissez exactement le même fichier sur les deux PC, puis recréez le salon.");
    });}
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
    void Begin(bool create,string invitationText){
        if(session!=null || disc==null || hashing)return;
        var profile=new PlayerInfo(nickname.Text,disc.Hash,disc.Length);
        Session current=null;current=new Session(t=>UI(()=>{if(session==current)SetStatus(t);}),()=>UI(()=>{if(session==current)RefreshLobby();}),t=>UI(()=>{if(session==current){Reset();SetStatus(t);}}),profile,disc);
        session=current;lastReport=current.Report;current.Host=create;RefreshLobby();
        current.Report.Write("role="+(create?"host":"guest")+" connection_requested");
        Task.Run(()=>{try{
            if(create){current.Create();UI(()=>{if(session==current && current.Bridge==null){invitation.Text=current.Invite.Encode();RefreshControls();SetStatus("Salon créé. Copiez l'invitation et envoyez-la à votre ami. Vous seul pourrez lancer le jeu.");}});}
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
    static string Friendly(Exception e){if(e is IOException)return e.Message;if(e is System.ComponentModel.Win32Exception)return "Windows n'a pas donné son autorisation. Réessayez et acceptez sa demande.";if(e is OperationCanceledException)return "Vérification annulée.";return "L'opération n'a pas abouti. Vérifiez votre connexion ou recréez le salon.";}
    void Reset(){var old=session;session=null;if(old!=null)Task.Run(()=>old.Dispose());RefreshLobby();SetStatus(disc!=null?"Créez un salon ou rejoignez votre ami.":"Choisissez votre disque pour commencer.");}
}
}
