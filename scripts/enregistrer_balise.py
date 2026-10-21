#!/usr/bin/env python3
"""Enregistrement d'une balise et génération de sa clé d'API.

Usage :
    python scripts/enregistrer_balise.py
    python scripts/enregistrer_balise.py --id bal-01 --nom "Bac Keur Massar" --hauteur 80 --seuil 40 --lon -17.31 --lat 14.78

Ce script génère une clé secrète aléatoire pour la balise, calcule son empreinte
SHA-256 (seule valeur stockée en base côté Supabase), et produit l'instruction SQL
à exécuter dans l'éditeur SQL de Supabase.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import secrets
import sys


def valider_id(balise_id: str) -> bool:
    return bool(re.match(r"^bal-[0-9]{2}$", balise_id))


def generer_cle() -> str:
    # Jeton sécurisé de 32 octets (43 caractères base64url)
    return "mbenn_" + secrets.token_urlsafe(32)


def hacher_cle(cle: str) -> str:
    return hashlib.sha256(cle.encode("utf-8")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Enregistre une nouvelle balise MbënnScan.")
    parser.add_argument("--id", help="Identifiant balise (ex: bal-01)")
    parser.add_argument("--nom", help="Nom descriptif ou emplacement")
    parser.add_argument("--hauteur", type=float, help="Hauteur capteur au-dessus du fond en cm (ex: 80)")
    parser.add_argument("--seuil", type=float, help="Seuil d'alerte en cm (ex: 40)")
    parser.add_argument("--lon", type=float, help="Longitude (ex: -17.312)")
    parser.add_argument("--lat", type=float, help="Latitude (ex: 14.785)")

    args = parser.parse_args()

    balise_id = args.id
    if not balise_id:
        try:
            balise_id = input("Identifiant de la balise (format bal-XX, ex: bal-01) [bal-01] : ").strip() or "bal-01"
        except (KeyboardInterrupt, EOFError):
            print("\nAnnulé.")
            return 1

    if not valider_id(balise_id):
        print(f"Erreur : '{balise_id}' ne respecte pas le format ^bal-[0-9]{{2}}$ (ex: bal-01, bal-02).", file=sys.stderr)
        return 1

    nom = args.nom
    if not nom:
        try:
            nom = input("Nom ou emplacement [Bac de démonstration] : ").strip() or "Bac de démonstration"
        except (KeyboardInterrupt, EOFError):
            print("\nAnnulé.")
            return 1

    hauteur = args.hauteur
    if hauteur is None:
        try:
            h_input = input("Hauteur capteur / fond du bac en cm [80.0] : ").strip() or "80.0"
            hauteur = float(h_input)
        except ValueError:
            print("Erreur : la hauteur doit être un nombre valide.", file=sys.stderr)
            return 1

    seuil = args.seuil
    if seuil is None:
        try:
            s_input = input("Seuil d'alerte en cm [40.0] : ").strip() or "40.0"
            seuil = float(s_input)
        except ValueError:
            print("Erreur : le seuil doit être un nombre valide.", file=sys.stderr)
            return 1

    lon = args.lon
    lat = args.lat
    if lon is None or lat is None:
        try:
            coords = input("Coordonnées GPS 'lon, lat' [-17.312, 14.785] : ").strip() or "-17.312, 14.785"
            parts = [float(x.strip()) for x in coords.split(",")]
            lon, lat = parts[0], parts[1]
        except Exception:
            lon, lat = -17.312, 14.785

    cle_api = generer_cle()
    cle_hash = hacher_cle(cle_api)

    sql = f"""-- Insertion / mise à jour de la balise {balise_id}
INSERT INTO balises (id, nom, geom, hauteur_capteur_cm, seuil_alerte_cm, cle_api_hash)
VALUES (
  '{balise_id}',
  '{nom.replace("'", "''")}',
  ST_SetSRID(ST_MakePoint({lon}, {lat}), 4326),
  {hauteur},
  {seuil},
  '{cle_hash}'
)
ON CONFLICT (id) DO UPDATE SET
  nom = EXCLUDED.nom,
  geom = EXCLUDED.geom,
  hauteur_capteur_cm = EXCLUDED.hauteur_capteur_cm,
  seuil_alerte_cm = EXCLUDED.seuil_alerte_cm,
  cle_api_hash = EXCLUDED.cle_api_hash;
"""

    print("\n" + "=" * 65)
    print(" BALISE ENREGISTRÉE AVEC SUCCÈS")
    print("=" * 65)
    print(f"ID Balise    : {balise_id}")
    print(f"Emplacement  : {nom}")
    print(f"Hauteur      : {hauteur} cm")
    print(f"Seuil alerte : {seuil} cm")
    print(f"Position GPS : Lon {lon}, Lat {lat}")
    print("-" * 65)
    print("⚠️  CLÉ D'API BALISE (À NE COPIER QU'UNE FOIS DANS secrets.h) :")
    print(f"\n    {cle_api}\n")
    print(f"Empreinte SHA-256 stockée en base : {cle_hash}")
    print("-" * 65)
    print("Instruction SQL à exécuter dans Supabase (SQL Editor) :\n")
    print(sql)
    print("=" * 65)
    print("👉 Dans firmware/include/secrets.h :")
    print(f'#define BALISE_ID      "{balise_id}"')
    print(f'#define BALISE_API_KEY "{cle_api}"')
    print("=" * 65 + "\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
