# supabase/

Base de données PostgreSQL + PostGIS, API REST automatique, temps réel pour la balise.

## Installer le schéma

1. Créer un projet sur supabase.com (région la plus proche : Europe).
2. *SQL Editor* → coller `schema.sql` → *Run*. Le script peut être relancé sans erreur.
3. Récupérer dans *Project Settings → API* : l'URL, la clé `anon` (pour `web/.env.local`) et la clé `service_role` (pour `pipeline/.env` uniquement).

Le schéma a été testé sur PostgreSQL 16 + PostGIS 3 avec les mocks : `zones_geojson()` renvoie un résultat conforme à `contracts/schemas/zones.schema.json`, le trigger calcule `niveau_cm`, le rôle `anon` lit tout sauf la clé des balises et ne peut rien écrire.

## Ce qui est exposé

| Lecture côté web | Contrat |
| --- | --- |
| `rpc/zones_geojson` | `zones.schema.json` |
| `zones`, `syntheses`, `indicateurs_saison` | `zone_detail.schema.json` (assemblé côté web) |
| `evenements`, `impacts_evenement` | `evenements.schema.json`, `impacts_evenement.schema.json` |
| `balises_publiques` (vue sans la clé) | `balises.schema.json` |
| `mesures` + Realtime | `mesures.schema.json` |

## Déploiement de l'Edge Function « mesure »

1. Lier votre projet distant Supabase :
   ```bash
   supabase link --project-ref <id-du-projet>
   ```
2. Déployer la fonction `mesure` (ne requiert pas de vérification JWT standard car la balise s'authentifie via sa clé SHA-256) :
   ```bash
   supabase functions deploy mesure --no-verify-jwt
   ```
3. L'URL d'ingestion sera : `https://<id-du-projet>.supabase.co/functions/v1/mesure`

## Enregistrer une balise

Utiliser le script pour créer une balise, générer sa clé secrète et insérer son empreinte SHA-256 :

```bash
python scripts/enregistrer_balise.py --id bal-01 --nom "Bac de démonstration" --hauteur 80 --seuil 40 --lon -17.312 --lat 14.785
```

Puis coller la requête SQL générée dans l'éditeur SQL Supabase, et reporter la clé dans `firmware/include/secrets.h`.
